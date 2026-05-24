#include "single_instance_guard.hpp"

#include "log_warning.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QDir>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QString>
#include <QTimer>

#if defined(Q_OS_WIN)
#include <lmcons.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>
#endif

namespace
{
/// Magic prefix on the wire so a junk client cannot hand us garbage
/// strings and have us treat them as file paths.
///
/// Note: this string is *not* version-suffixed. The schema-version
/// axis is the explicit `WIRE_VERSION` byte that follows the magic.
/// The previous form `"STRUCTLOGV1"` confusingly mixed the two,
/// making a future protocol bump look like a magic change. Tests pin
/// the value in [test_single_instance_guard.cpp].
constexpr char WIRE_MAGIC[] = "STRUCTLOG";

/// Number of bytes the magic occupies on the wire after the
/// `QDataStream`-encoded `QByteArray` length prefix. Used by
/// `HandleNewConnection` to peek the magic *before* paying the
/// allocation cost of a `QStringList` decode -- a hostile peer
/// pre-fix could trigger a 1 MiB allocation by sending well-formed
/// length prefixes around garbage payloads.
/// `QDataStream::Qt_6_0` writes `QByteArray` as a `quint32` length
/// followed by the bytes (no NUL terminator). Length prefix is 4
/// bytes; magic is 9 bytes; version byte is 1 byte -> 14-byte peek
/// window before we trust the rest of the frame.
constexpr int MAGIC_BYTES = sizeof(WIRE_MAGIC) - 1;
constexpr int MAGIC_FRAME_PEEK_BYTES =
    static_cast<int>(sizeof(quint32)) + MAGIC_BYTES + static_cast<int>(sizeof(quint8));

/// Wire format version, serialised as a `quint8` immediately after
/// `WIRE_MAGIC`. Bump on any breaking change to the payload schema
/// so a primary running an updated build can reject older
/// secondaries cleanly (instead of silently misinterpreting their
/// payload). The primary checks that the received version is in
/// `[WIRE_VERSION_MIN_SUPPORTED, WIRE_VERSION_MAX_SUPPORTED]` so a
/// newer secondary against an older primary fails fast rather
/// than producing surprising behaviour, and an older secondary
/// whose schema we no longer accept can be rejected by bumping
/// `WIRE_VERSION_MIN_SUPPORTED` past it.
///
/// Version 2 (current): magic + version + QStringList files +
/// quint32 truncatedCount. The trailing count carries how many
/// files the secondary held but could not send because it hit
/// `MAX_FORWARDED_FILES`, so the primary can surface a truncation
/// warning on the spawned window. Version 1 lacked the count
/// (silently truncated, no UI surface); we drop backward
/// compatibility per the review plan -- old primaries cannot
/// understand new secondaries and vice versa, but
/// single-instance coordination only matters within a single
/// rolling-installed product so a mixed-version pairing is not a
/// realistic deployment shape.
constexpr quint8 WIRE_VERSION = 2;
constexpr quint8 WIRE_VERSION_MIN_SUPPORTED = 2;
constexpr quint8 WIRE_VERSION_MAX_SUPPORTED = 2;

/// Connection / write timeouts. Tight on purpose: a secondary that
/// cannot reach the primary within a second should fall back to
/// becoming its own primary rather than block the user's launch.
constexpr int CONNECT_TIMEOUT_MS = 1000;
constexpr int WRITE_TIMEOUT_MS = 1000;

/// Server-side cap on how long an incoming connection may sit
/// without completing a frame. A peer that connects but never
/// finishes the `QDataStream` header (and never disconnects) would
/// otherwise pin its socket + per-connection buffer for the
/// lifetime of the primary. 5 s is well above the worst-case
/// network-loopback round trip but well below "noticeably long".
constexpr int CONNECTION_IDLE_TIMEOUT_MS = 5000;

/// Hard cap on inbound payload size so a malformed / hostile peer
/// cannot OOM the primary by sending a giant stream. 1 MiB is far
/// above what a CLI invocation would ever carry.
constexpr qint64 MAX_PAYLOAD_BYTES = 1024 * 1024;

/// Cap on the number of file paths a secondary may forward in a
/// single launch. Shared by the secondary (truncates before send +
/// warns the user) and the primary (truncates post-decode as a
/// belt-and-braces against a malformed / hostile peer that ignores
/// the documented limit). 256 comfortably covers shell glob
/// expansions while staying well below any value at which the
/// open-files flow stops being user-meaningful.
constexpr int MAX_FORWARDED_FILES = 256;

/// `setMaxPendingConnections` ceiling. Default is `30` which a CI
/// script firing a burst of background launches can saturate within
/// a few hundred milliseconds; raising the bound lets the primary
/// drain the burst rather than refuse late arrivals.
constexpr int MAX_PENDING_CONNECTIONS = 1024;

/// Acquisition timeout for the cross-process takeover lock. The
/// lock spans the `removeServer` + `listen` pair on the would-be
/// primary so two simultaneously-launched processes cannot both
/// proceed past their probe-disconnect step. Short because the
/// guarded section is essentially zero-I/O.
constexpr int TAKEOVER_LOCK_TIMEOUT_MS = 1000;

/// Best-effort current-user identifier used as salt for the
/// per-user socket name. Falls back to the platform API when the
/// `USER` / `USERNAME` environment variable is empty (this is
/// possible under `sudo -u`, inside container runtimes, and on
/// stripped-down service accounts).
QString CurrentUserId()
{
    const QString env = QProcessEnvironment::systemEnvironment().value(
#if defined(Q_OS_WIN)
        QStringLiteral("USERNAME")
#else
        QStringLiteral("USER")
#endif
    );
    if (!env.isEmpty())
    {
        return env;
    }
#if defined(Q_OS_WIN)
    wchar_t name[UNLEN + 1];
    DWORD size = UNLEN + 1;
    if (GetUserNameW(name, &size) && size > 0)
    {
        return QString::fromWCharArray(name, static_cast<int>(size) - 1);
    }
#else
    if (struct passwd *pwd = getpwuid(getuid()); pwd && pwd->pw_name)
    {
        return QString::fromLocal8Bit(pwd->pw_name);
    }
#endif
    return QStringLiteral("unknown");
}

/// Path used for the `probe -> listen` takeover lock. We deliberately
/// root this in `TempLocation` (writable, world-readable, survives
/// reboots cleanly via OS-managed cleanup) instead of next to the
/// socket file, because on Linux some `QStandardPaths::RuntimeLocation`
/// directories are sticky to a single login session.
QString TakeoverLockPath(const QString &socketName)
{
    QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (dir.isEmpty())
    {
        dir = QDir::tempPath();
    }
    return QDir(dir).filePath(socketName + QStringLiteral(".takeover.lock"));
}
} // namespace

SingleInstanceGuard::SingleInstanceGuard(QObject *parent)
    : QObject(parent), mSocketName(DefaultSocketName())
{
}

SingleInstanceGuard::~SingleInstanceGuard()
{
    if (mServer)
    {
        // Disconnect the `newConnection` slot before tearing down to
        // make sure a connection that arrives while we are mid-cleanup
        // does not race the `mConnections` clear. `QObject` would
        // disconnect automatically on destruction, but the explicit
        // `disconnect` plus `close` pair ordering is the documented
        // shutdown sequence for `QLocalServer`.
        disconnect(mServer.get(), nullptr, this, nullptr);
        // Tear down any pending per-connection state before closing
        // the server. Each `QLocalSocket *` in `mConnections` is
        // owned by its `deleteLater`-on-disconnected chain; we only
        // strip the bookkeeping side here.
        const auto sockets = mConnections.keys();
        for (QLocalSocket *socket : sockets)
        {
            DropConnection(socket);
        }
        // `QLocalServer::close()` only stops accepting; on Linux the
        // socket file lingers in `/tmp` (or `$XDG_RUNTIME_DIR`) until
        // we explicitly call `removeServer`. Without this, a clean
        // exit followed by a fast relaunch sometimes finds the stale
        // file and `connectToServer` succeeds against a dead peer.
        mServer->close();
        QLocalServer::removeServer(mSocketName);
    }
}

void SingleInstanceGuard::SetSocketNameForTest(const QString &name)
{
    // Must be called before `TryAcquire`: changing the socket name
    // after the server has been bound would leave `mServer` listening
    // on the original path while `mSocketName` claims a new one, so
    // the next `TryAcquire` would either silently zombie the live
    // server (re-bind on a different name) or, on Linux, unlink a
    // stale path that does not exist. In debug builds we keep the
    // historical assert so tests catch the misuse loudly; in release
    // builds we degrade to a `qWarning` + early return rather than
    // proceeding into the inconsistent state.
    Q_ASSERT(mServer == nullptr);
    if (mServer != nullptr)
    {
        qWarning() << "SingleInstanceGuard::SetSocketNameForTest ignored after TryAcquire; "
                      "the existing server remains bound to the previous socket name";
        return;
    }
    mSocketName = name;
}

bool SingleInstanceGuard::TryAcquire(const QStringList &forwardFiles, bool allowNewInstance)
{
    if (allowNewInstance)
    {
        // Bypass: run fully uncoordinated. We deliberately do NOT
        // call `QLocalServer::removeServer` / `listen` here -- doing
        // so would unlink the canonical socket file on Linux and
        // silently zombie the already-running primary (its listening
        // FD survives but new clients cannot resolve the path).
        // Trade-off: this process cannot accept its own future
        // forwards, but plain double-clicks still reach the
        // canonical primary, which is the documented intent of the
        // `--new-instance` escape hatch ("explicitly running
        // side-by-side processes").
        return true;
    }

    // Truncate before serialisation so a pathological shell glob
    // never overruns `MAX_PAYLOAD_BYTES` on the wire. Without this,
    // an oversized payload would land at the primary, which would
    // detect the overrun and disconnect -- and the user would see
    // no window open at all because the secondary exits as soon as
    // the forward returns. Mirrors the post-decode cap in
    // `HandleNewConnection` so the limit is symmetric.
    //
    // `truncatedCount` carries the dropped surplus so the primary
    // can surface a "we silently dropped N files" hint on the
    // spawned window. Pre-fix the only signal was the `qWarning`
    // below, which is invisible to users who launched from the GUI
    // shell -- the dropped files just never appeared.
    QStringList trimmed = forwardFiles;
    quint32 truncatedCount = 0;
    if (trimmed.size() > MAX_FORWARDED_FILES)
    {
        truncatedCount = static_cast<quint32>(trimmed.size() - MAX_FORWARDED_FILES);
        qWarning() << "SingleInstanceGuard: truncating forwarded file list from" << trimmed.size() << "to"
                   << MAX_FORWARDED_FILES << "entries (" << truncatedCount
                   << "dropped); the primary will be notified to surface a warning.";
        trimmed = trimmed.mid(0, MAX_FORWARDED_FILES);
    }

    // Serialise the payload up-front so we can apply the
    // `MAX_PAYLOAD_BYTES` cap *before* committing to the forward
    // path. Pre-fix, an oversized payload would only be detected
    // post-decode on the primary side, by which time the secondary
    // had already paid the round-trip latency cost (and the user
    // would see no window because the secondary exits regardless of
    // forward success). Now we fall through to the listen branch on
    // overrun so the user always gets *some* window.
    //
    // Byte-budget tail trim: `MAX_FORWARDED_FILES` (count cap) alone
    // does not guarantee the encoded payload fits under
    // `MAX_PAYLOAD_BYTES`. On platforms with very long paths (Linux
    // PATH_MAX = 4096, deep nested workspaces, ...) 256 entries can
    // exceed 1 MiB. Pop entries from the tail and re-serialise until
    // either the payload fits or the list is empty; the popped
    // surplus is folded into `truncatedCount` so the primary's
    // spawned-window hint accounts for both forms of truncation.
    // Worst case is bounded by `MAX_FORWARDED_FILES` re-serialise
    // iterations (each pops at least one entry); typical payloads
    // succeed on the first attempt and pay zero retries.
    auto serialise = [&trimmed, &truncatedCount]() {
        QByteArray buf;
        QDataStream stream(&buf, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_0);
        stream << QByteArray(WIRE_MAGIC);
        stream << static_cast<quint8>(WIRE_VERSION);
        stream << trimmed;
        stream << truncatedCount;
        return buf;
    };
    QByteArray payload = serialise();
    while (payload.size() > MAX_PAYLOAD_BYTES && !trimmed.isEmpty())
    {
        trimmed.removeLast();
        ++truncatedCount;
        payload = serialise();
    }
    // `payloadFits` is now "the bytes fit AND we have something to
    // forward". An originally-empty `forwardFiles` is a valid
    // operation (the secondary asks the primary to spawn an empty
    // window) -- the second clause only rejects the pathological
    // "tail trim emptied a non-empty input but the bytes still
    // overrun" case.
    const bool payloadFits = payload.size() <= MAX_PAYLOAD_BYTES && (forwardFiles.isEmpty() || !trimmed.isEmpty());
    if (!payloadFits)
    {
        logapp::LogWarning() << "SingleInstanceGuard: forward payload" << payload.size() << "bytes exceeds cap"
                             << MAX_PAYLOAD_BYTES
                             << "even after tail trim; falling through to listen so the user still gets a window.";
    }

    auto forwardTo = [&payload](QLocalSocket &probe) -> bool {
        const qint64 written = probe.write(payload);
        if (written != payload.size())
        {
            logapp::LogWarning() << "SingleInstanceGuard: short write forwarding to primary; wrote" << written << "of"
                                 << payload.size() << "bytes; error:" << probe.error() << probe.errorString();
            return false;
        }
        // `flush()` and `waitForBytesWritten()` legitimately return
        // `false` when the OS has already drained the user-space
        // buffer (common on Windows named pipes, where `write` is
        // synchronous), so we ignore their return values here.
        probe.flush();
        probe.waitForBytesWritten(WRITE_TIMEOUT_MS);
        // Wait for the primary to ack by tearing the connection
        // down on its end. Without this the secondary process
        // may exit before the primary drains the OS-level pipe
        // buffer (observed on Windows named pipes). `false` here
        // is fine when the primary already closed before we got to
        // wait (small payloads complete sub-millisecond) or when
        // the OS pipe buffer is still draining on a same-process
        // peer; the only failure-relevant signal we have is
        // `state() == ConnectedState` *and* a non-empty
        // `bytesToWrite()` after the wait. Either alone (already
        // disconnected, or transient send-queue activity) is fine.
        const bool ack = probe.waitForDisconnected(WRITE_TIMEOUT_MS);
        if (!ack && probe.state() == QLocalSocket::ConnectedState && probe.bytesToWrite() > 0)
        {
            logapp::LogWarning() << "SingleInstanceGuard: forward stalled with" << probe.bytesToWrite()
                                 << "bytes still pending; error:" << probe.error() << probe.errorString();
            return false;
        }
        return true;
    };

    // Step 1: try to connect as a secondary. If the connect
    // succeeds and the payload fits, forward. Forward failures
    // (short write, flush error, disconnected timeout) fall through
    // to the listen branch so the user always gets *some* window.
    if (payloadFits)
    {
        QLocalSocket probe;
        probe.connectToServer(mSocketName);
        if (probe.waitForConnected(CONNECT_TIMEOUT_MS))
        {
            if (forwardTo(probe))
            {
                return false;
            }
            // Forward attempt failed; fall through to listen.
        }
    }

    // Step 2: become the primary. The `removeServer` + `listen`
    // pair is the race-prone region: two simultaneously-launching
    // secondaries that both failed to connect in step 1 would
    // otherwise both call `removeServer` (unlinking a freshly-bound
    // socket file the other just created) and then race on
    // `listen`. A cross-process `QLockFile` in `TempLocation`
    // serialises the section so only one becomes the primary; the
    // loser falls back through `connectToServer` and forwards.
    QLockFile takeover(TakeoverLockPath(mSocketName));
    takeover.setStaleLockTime(0);
    if (!takeover.tryLock(TAKEOVER_LOCK_TIMEOUT_MS))
    {
        logapp::LogWarning() << "SingleInstanceGuard: takeover lock contended; retrying forward path";
        QLocalSocket retry;
        retry.connectToServer(mSocketName);
        if (retry.waitForConnected(CONNECT_TIMEOUT_MS))
        {
            if (payloadFits && forwardTo(retry))
            {
                return false;
            }
        }
        // Even the retry failed -- run uncoordinated rather than
        // hang the user's launch.
        return true;
    }

    // `removeServer` is a no-op when the socket name is free.
    QLocalServer::removeServer(mSocketName);
    // No QObject parent: the `unique_ptr` is the sole owner. Parenting
    // to `this` *and* unique_ptr-owning was a latent double-delete
    // footgun -- safe today only because QObject's child-list removal
    // races the unique_ptr destructor in the right order, but any
    // future reparent / early reset would break that invariant.
    mServer = std::make_unique<QLocalServer>();
    mServer->setSocketOptions(QLocalServer::UserAccessOption);
    // Wire the slot before `listen()`. Qt's contract is that the
    // signal can fire from inside `listen()` itself on platforms
    // where the OS surfaces queued client connections synchronously
    // (observed on macOS Unix domain sockets under load); wiring
    // first is the documented-safe order.
    connect(mServer.get(), &QLocalServer::newConnection, this, &SingleInstanceGuard::HandleNewConnection);
    if (!mServer->listen(mSocketName))
    {
        // Another secondary won the race in the gap between our
        // `connectToServer` and our `listen` (rare with the takeover
        // lock, but still possible if a third process started up
        // and bound between our `removeServer` and `listen`). Try
        // the forward path once more before giving up and running as
        // a detached primary.
        QLocalSocket retry;
        retry.connectToServer(mSocketName);
        if (retry.waitForConnected(CONNECT_TIMEOUT_MS))
        {
            const bool forwarded = payloadFits && forwardTo(retry);
            mServer.reset();
            takeover.unlock();
            if (forwarded)
            {
                return false;
            }
            // Fall back to running uncoordinated; better than no
            // window.
            return true;
        }
        // Stranger errors: we cannot bind the socket *and* nothing
        // is listening. Run as a primary without coordination --
        // secondaries will misroute, but at least the user gets a
        // window. The cross-process `recents.lock` taken by every
        // `SessionHistoryManager` mutator keeps the recents index
        // consistent even in this degraded state.
        mServer.reset();
        takeover.unlock();
        return true;
    }
    mServer->setMaxPendingConnections(MAX_PENDING_CONNECTIONS);
    takeover.unlock();
    return true;
}

void SingleInstanceGuard::DropConnection(QLocalSocket *socket)
{
    auto it = mConnections.find(socket);
    if (it == mConnections.end())
    {
        return;
    }
    if (it->idleTimer != nullptr)
    {
        it->idleTimer->stop();
        // Timer is parented to the socket and dies with it; we just
        // null the bookkeeping copy here so a double `DropConnection`
        // (e.g. destructor after `disconnected`) is a no-op.
    }
    mConnections.erase(it);
}

void SingleInstanceGuard::HandleNewConnection()
{
    while (QLocalSocket *socket = mServer ? mServer->nextPendingConnection() : nullptr)
    {
        ConnState &state = mConnections[socket];
        // Per-connection idle watchdog. A peer that connects, sits
        // silent, and never disconnects would otherwise leak its
        // socket + buffer until the primary exits. The pre-fix used
        // a one-shot `QTimer::singleShot` armed from accept-time,
        // which would close even a healthy-but-slow connection at
        // `CONNECTION_IDLE_TIMEOUT_MS` regardless of activity. The
        // member timer is `restart`ed inside the `readyRead` slot so
        // "idle" actually means "no activity for N ms".
        auto *timer = new QTimer(socket);
        timer->setSingleShot(true);
        timer->setInterval(CONNECTION_IDLE_TIMEOUT_MS);
        connect(timer, &QTimer::timeout, socket, [socket]() {
            if (socket->state() != QLocalSocket::UnconnectedState)
            {
                socket->disconnectFromServer();
            }
        });
        state.idleTimer = timer;
        timer->start();

        connect(socket, &QLocalSocket::disconnected, this, [this, socket]() {
            DropConnection(socket);
            socket->deleteLater();
        });

        // Re-parse the buffer from byte 0 on every `readyRead`.
        // `QDataStream` reads through the buffer without mutating
        // it, so each pass keeps the same wire bytes across
        // invocations until a frame completes. Total work is
        // bounded by `MAX_PAYLOAD_BYTES`, so the per-readyRead cost
        // is constant in the worst case.
        auto tryDecode = [this, socket]() {
            auto it = mConnections.find(socket);
            if (it == mConnections.end())
            {
                return; // Disconnected between signal and slot.
            }
            QByteArray &buffer = it->buffer;
            buffer.append(socket->readAll());
            if (buffer.size() > MAX_PAYLOAD_BYTES)
            {
                logapp::LogWarning() << "SingleInstanceGuard: dropping peer that exceeded MAX_PAYLOAD_BYTES";
                socket->disconnectFromServer();
                return;
            }
            // Idle timer resets on every readyRead so a healthy
            // peer that drips bytes does not get kicked.
            if (it->idleTimer != nullptr)
            {
                it->idleTimer->start();
            }

            // Hoist the magic + version check ahead of the full
            // payload decode. Pre-fix, a hostile peer could send a
            // well-formed `QByteArray` length prefix around junk
            // bytes and we'd pay the allocation cost of a full
            // `QStringList` decode (up to `MAX_PAYLOAD_BYTES`)
            // before discovering the magic was wrong. The new peek
            // window (`MAGIC_FRAME_PEEK_BYTES`) bounds the wasted
            // work to ~14 bytes.
            if (buffer.size() >= MAGIC_FRAME_PEEK_BYTES)
            {
                QDataStream magicPeek(buffer);
                magicPeek.setVersion(QDataStream::Qt_6_0);
                QByteArray magic;
                magicPeek >> magic;
                quint8 version = 0;
                magicPeek >> version;
                if (magicPeek.status() != QDataStream::Ok || magic != QByteArray(WIRE_MAGIC) ||
                    version < WIRE_VERSION_MIN_SUPPORTED || version > WIRE_VERSION_MAX_SUPPORTED)
                {
                    logapp::LogWarning() << "SingleInstanceGuard: rejecting peer with bad magic or unsupported version"
                                         << version;
                    socket->disconnectFromServer();
                    return;
                }
            }

            QDataStream peek(buffer);
            peek.setVersion(QDataStream::Qt_6_0);
            QByteArray magic;
            peek >> magic;
            quint8 version = 0;
            peek >> version;
            QStringList files;
            peek >> files;
            quint32 truncatedCount = 0;
            peek >> truncatedCount;
            if (peek.status() != QDataStream::Ok)
            {
                return; // Wait for more bytes.
            }
            // Belt-and-braces post-decode cap matching the
            // secondary-side trim in `TryAcquire`. `MAX_PAYLOAD_BYTES`
            // already bounds the wire frame; this guards against a
            // peer that ignored the documented limit and packed more
            // files into a smaller payload (e.g. duplicate paths).
            // The trimmed list still opens cleanly; the surplus is
            // folded into `truncatedCount` so the user-facing message
            // accounts for it.
            if (files.size() > MAX_FORWARDED_FILES)
            {
                const auto overrun = static_cast<quint32>(files.size() - MAX_FORWARDED_FILES);
                truncatedCount += overrun;
                files = files.mid(0, MAX_FORWARDED_FILES);
            }
            emit openWindowRequested(files, static_cast<int>(truncatedCount));
            socket->disconnectFromServer();
        };

        connect(socket, &QLocalSocket::readyRead, this, tryDecode);

        // On Windows named pipes the data may already have arrived
        // by the time `newConnection` fires; the readyRead signal
        // does not fire retroactively for already-buffered bytes
        // when wired this late. Drain synchronously *if* bytes are
        // already buffered, but never block on `waitForReadyRead`:
        // a slow / silent peer would otherwise freeze the primary's
        // GUI thread for up to `WRITE_TIMEOUT_MS` per connection
        // (a script firing N background launches multiplies the
        // freeze by N). When bytes aren't already available, we
        // rely on the wired-up `readyRead` signal to fire from the
        // event loop, with the per-connection idle watchdog
        // (`CONNECTION_IDLE_TIMEOUT_MS`) tearing down any peer that
        // connects but never sends anything.
        if (socket->bytesAvailable() > 0)
        {
            tryDecode();
        }
    }
}

QString SingleInstanceGuard::DefaultSocketName()
{
    // Mix in `applicationName` so a nightly / stable / sandboxed
    // installation of the same product can coexist without sharing
    // a primary. Defaults to the binary name when the embedding
    // application forgot to call `setApplicationName`, which still
    // gives different names to two unrelated apps. Empty `appId`
    // would collapse the hash to just the user salt, which is a
    // configuration bug we want to surface in debug builds.
    const QString appId = QCoreApplication::applicationName();
    Q_ASSERT_X(
        !appId.isEmpty(),
        "SingleInstanceGuard::DefaultSocketName",
        "QCoreApplication::applicationName() is empty; call setApplicationName before constructing the guard"
    );

    // Per-user salt so two users on the same host get independent
    // primaries (Linux / macOS file-mode sockets honour
    // UserAccessOption, but the name still has to differ to avoid a
    // collision under e.g. `sudo -u other`). Falls back to a
    // platform-API lookup when the env var is unset (sudo, service
    // accounts, container runtimes).
    const QString user = CurrentUserId();
    const QByteArray salt = (user + QLatin1Char('|') + appId).toUtf8();

    // Hash to a fixed length so the resulting name is bounded
    // regardless of input; prefix with the app id so multiple
    // products on the same host coexist.
    const QByteArray digest = QCryptographicHash::hash(salt, QCryptographicHash::Sha1).toHex().left(16);
    return QStringLiteral("structured-log-viewer.") + QString::fromLatin1(digest);
}
