#include "single_instance_guard.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDataStream>
#include <QLocalSocket>
#include <QProcessEnvironment>
#include <QString>
#include <QTimer>
#include <QVariant>

namespace
{
/// Magic prefix on the wire so a junk client cannot hand us garbage
/// strings and have us treat them as file paths.
constexpr char WIRE_MAGIC[] = "STRUCTLOGV1";

/// Wire format version, serialised as a `quint8` immediately after
/// `WIRE_MAGIC`. Bump on any breaking change to the payload schema
/// so a primary running an updated build can reject older
/// secondaries cleanly (instead of silently misinterpreting their
/// payload). Today only version 1 exists; future revs may add
/// fields after the file list. The primary checks that the
/// received version is in
/// `[WIRE_VERSION_MIN_SUPPORTED, WIRE_VERSION_MAX_SUPPORTED]` so a
/// newer secondary against an older primary fails fast rather
/// than producing surprising behaviour, and an older secondary
/// whose schema we no longer accept can be rejected by bumping
/// `WIRE_VERSION_MIN_SUPPORTED` past it.
constexpr quint8 WIRE_VERSION = 1;
constexpr quint8 WIRE_VERSION_MIN_SUPPORTED = 1;
constexpr quint8 WIRE_VERSION_MAX_SUPPORTED = 1;

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
} // namespace

SingleInstanceGuard::SingleInstanceGuard(QObject *parent)
    : QObject(parent), mSocketName(DefaultSocketName())
{
}

SingleInstanceGuard::~SingleInstanceGuard()
{
    if (mServer)
    {
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
    QStringList trimmed = forwardFiles;
    if (trimmed.size() > MAX_FORWARDED_FILES)
    {
        qWarning() << "SingleInstanceGuard: truncating forwarded file list from" << trimmed.size() << "to"
                   << MAX_FORWARDED_FILES << "entries; the primary will not see the surplus.";
        trimmed = trimmed.mid(0, MAX_FORWARDED_FILES);
    }

    auto forwardTo = [&trimmed](QLocalSocket &probe) {
        QByteArray payload;
        QDataStream stream(&payload, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_0);
        stream << QByteArray(WIRE_MAGIC);
        // Version byte sits between the magic and the payload. A
        // pre-versioned (no-byte) secondary is impossible today
        // (same binary on both sides), but the byte gives future
        // schema changes a clean upgrade story: a v2 secondary
        // talking to a v1 primary is rejected via the
        // `WIRE_VERSION_MAX_SUPPORTED` check on the read side.
        stream << static_cast<quint8>(WIRE_VERSION);
        stream << trimmed;
        probe.write(payload);
        probe.flush();
        probe.waitForBytesWritten(WRITE_TIMEOUT_MS);
        // Wait for the primary to ack by tearing the connection
        // down on its end. Without this the secondary process
        // may exit before the primary drains the OS-level pipe
        // buffer (observed on Windows named pipes).
        probe.waitForDisconnected(WRITE_TIMEOUT_MS);
    };

    // Step 1: try to connect as a secondary. If the connect
    // succeeds, the primary is up and we forward.
    {
        QLocalSocket probe;
        probe.connectToServer(mSocketName);
        if (probe.waitForConnected(CONNECT_TIMEOUT_MS))
        {
            forwardTo(probe);
            return false;
        }
    }

    // Step 2: become the primary. Remove any stale socket file first
    // (Linux leaves them behind when a previous process crashed).
    // `removeServer` is a no-op when the socket name is free.
    QLocalServer::removeServer(mSocketName);
    // No QObject parent: the `unique_ptr` is the sole owner. Parenting
    // to `this` *and* unique_ptr-owning was a latent double-delete
    // footgun -- safe today only because QObject's child-list removal
    // races the unique_ptr destructor in the right order, but any
    // future reparent / early reset would break that invariant.
    mServer = std::make_unique<QLocalServer>();
    mServer->setSocketOptions(QLocalServer::UserAccessOption);
    if (!mServer->listen(mSocketName))
    {
        // Last-ditch: another secondary won the race in the gap
        // between our `connectToServer` and our `listen`. Try the
        // forward path once more before giving up and running as a
        // detached primary.
        QLocalSocket retry;
        retry.connectToServer(mSocketName);
        if (retry.waitForConnected(CONNECT_TIMEOUT_MS))
        {
            forwardTo(retry);
            mServer.reset();
            return false;
        }
        // Stranger errors: we cannot bind the socket *and* nothing
        // is listening. Run as a primary without coordination --
        // secondaries will misroute, but at least the user gets a
        // window. The cross-process `recents.lock` taken by every
        // `SessionHistoryManager` mutator keeps the recents index
        // consistent even in this degraded state.
        mServer.reset();
        return true;
    }

    connect(mServer.get(), &QLocalServer::newConnection, this, &SingleInstanceGuard::HandleNewConnection);
    return true;
}

void SingleInstanceGuard::HandleNewConnection()
{
    while (QLocalSocket *socket = mServer ? mServer->nextPendingConnection() : nullptr)
    {
        connect(socket, &QLocalSocket::disconnected, socket, &QLocalSocket::deleteLater);

        // Per-connection rolling buffer. Kept on the socket via
        // dynamic property so the lambda can outlive any single
        // `readyRead` invocation without us tracking sockets in a
        // map. `QByteArray` is value-typed and trivially copied
        // into / out of `QVariant`.
        //
        // Lifetime: an incomplete frame (peer disconnects mid-send,
        // never finishes the `QDataStream` header) is collected into
        // this buffer and then dropped when the socket emits
        // `disconnected` -- the `deleteLater` above tears the
        // `QLocalSocket` down, and the `QVariant`-owned `QByteArray`
        // dies with it. No long-lived leak.
        auto tryDecode = [this, socket]() {
            QByteArray buffer = socket->property("structlog_buf").toByteArray();
            buffer.append(socket->readAll());
            if (buffer.size() > MAX_PAYLOAD_BYTES)
            {
                socket->disconnectFromServer();
                return;
            }

            // Re-parse the buffer from byte 0 on every `readyRead`.
            // `QDataStream` reads through `&buffer` without mutating
            // the QByteArray, so the saved-and-restored buffer keeps
            // the same wire bytes across invocations. Total work is
            // bounded by `MAX_PAYLOAD_BYTES`, so the per-readyRead
            // cost is constant in the worst case.
            QDataStream peek(&buffer, QIODevice::ReadOnly);
            peek.setVersion(QDataStream::Qt_6_0);
            QByteArray magic;
            peek >> magic;
            quint8 version = 0;
            peek >> version;
            QStringList files;
            peek >> files;
            if (peek.status() == QDataStream::Ok)
            {
                // `version > WIRE_VERSION_MAX_SUPPORTED` means a
                // newer secondary is talking to this primary; we
                // refuse rather than guess at the payload schema.
                // `version < WIRE_VERSION_MIN_SUPPORTED` is the
                // symmetric direction -- we will use this in the
                // future to drop support for retired wire versions
                // without sprinkling magic numbers across the
                // codebase. The connection is still drained /
                // disconnected below so the secondary's
                // `waitForDisconnected` returns promptly and the
                // user gets a fresh primary in the secondary
                // instead of a hung forward attempt.
                if (magic == QByteArray(WIRE_MAGIC)
                    && version >= WIRE_VERSION_MIN_SUPPORTED
                    && version <= WIRE_VERSION_MAX_SUPPORTED)
                {
                    // Belt-and-braces post-decode cap matching the
                    // secondary-side trim in `TryAcquire::forwardTo`.
                    // `MAX_PAYLOAD_BYTES` already bounds the wire
                    // frame; this guards against a peer that ignored
                    // the documented limit and packed more files into
                    // a smaller payload (e.g. compressing identical
                    // paths). The trimmed list still opens cleanly --
                    // the surplus is silently dropped.
                    if (files.size() > MAX_FORWARDED_FILES)
                    {
                        files = files.mid(0, MAX_FORWARDED_FILES);
                    }
                    emit openWindowRequested(files);
                }
                socket->disconnectFromServer();
                return;
            }
            socket->setProperty("structlog_buf", buffer);
        };

        connect(socket, &QLocalSocket::readyRead, this, tryDecode);

        // Per-connection idle watchdog. A peer that connects, sits
        // silent, and never disconnects would otherwise leak its
        // socket + buffer property until process exit. The timer is
        // parented to the socket so it dies with it; the lambda
        // disconnects the peer, which fires `disconnected` and the
        // matching `deleteLater` above.
        QTimer::singleShot(CONNECTION_IDLE_TIMEOUT_MS, socket, [socket]() {
            if (socket->state() != QLocalSocket::UnconnectedState)
            {
                socket->disconnectFromServer();
            }
        });

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
    // Per-user salt so two users on the same host get independent
    // primaries (Linux / macOS file-mode sockets honour
    // UserAccessOption, but the name still has to differ to avoid a
    // collision under e.g. `sudo -u other`).
    const QString user = QProcessEnvironment::systemEnvironment().value(
#if defined(Q_OS_WIN)
        QStringLiteral("USERNAME")
#else
        QStringLiteral("USER")
#endif
    );

    // Mix in `applicationName` so a nightly / stable / sandboxed
    // installation of the same product can coexist without sharing
    // a primary. Defaults to the binary name when the embedding
    // application forgot to call `setApplicationName`, which still
    // gives different names to two unrelated apps. The user is
    // included so a single host with multiple logged-in users
    // routes each user's secondaries to their own primary.
    const QString appId = QCoreApplication::applicationName();
    const QByteArray salt = (user + QLatin1Char('|') + appId).toUtf8();

    // Hash to a fixed length so the resulting name is bounded
    // regardless of input; prefix with the app id so multiple
    // products on the same host coexist.
    const QByteArray digest = QCryptographicHash::hash(salt, QCryptographicHash::Sha1).toHex().left(16);
    return QStringLiteral("structured-log-viewer.") + QString::fromLatin1(digest);
}
