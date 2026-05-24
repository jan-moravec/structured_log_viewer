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

#ifdef Q_OS_WIN
#include <lmcons.h>
#include <windows.h>
#else
#include <sys/types.h>
#include <pwd.h>
#include <unistd.h>
#endif

namespace
{
/// Magic prefix on the wire. Version follows as a separate byte.
constexpr char WIRE_MAGIC[] = "STRUCTLOG";

/// Peek window for magic + version: `QDataStream::Qt_6_0` writes a
/// `QByteArray` as a `quint32` length prefix + bytes, plus the
/// trailing `quint8` version = 4 + 9 + 1 = 14 bytes. Lets us
/// reject a junk peer before allocating for the full decode.
constexpr int MAGIC_BYTES = sizeof(WIRE_MAGIC) - 1;
constexpr int MAGIC_FRAME_PEEK_BYTES =
    static_cast<int>(sizeof(quint32)) + MAGIC_BYTES + static_cast<int>(sizeof(quint8));

/// Wire schema version after `WIRE_MAGIC`. Bump on payload changes.
/// Version 2: magic + version + `QStringList` files +
/// `quint32 truncatedCount`.
constexpr quint8 WIRE_VERSION = 2;
constexpr quint8 WIRE_VERSION_MIN_SUPPORTED = 2;
constexpr quint8 WIRE_VERSION_MAX_SUPPORTED = 2;

/// Connect / write timeouts. Tight so an unreachable primary
/// degrades to running uncoordinated rather than stalling launch.
constexpr int CONNECT_TIMEOUT_MS = 1000;
constexpr int WRITE_TIMEOUT_MS = 1000;

/// Drop an inbound connection idle for this long; prevents a half-
/// open peer from pinning its socket forever.
constexpr int CONNECTION_IDLE_TIMEOUT_MS = 5000;

/// Inbound payload cap (defence against OOM by hostile peer).
constexpr qint64 MAX_PAYLOAD_BYTES = 1024 * 1024;

/// Per-launch file-count cap; enforced on both sides.
constexpr int MAX_FORWARDED_FILES = 256;

/// `setMaxPendingConnections` ceiling (default 30 saturates in
/// CI bursts).
constexpr int MAX_PENDING_CONNECTIONS = 1024;

/// Timeout for the takeover lock around `removeServer` + `listen`.
constexpr int TAKEOVER_LOCK_TIMEOUT_MS = 1000;

/// Salt for the socket name. Falls back to the platform API when
/// `USER` / `USERNAME` is unset (sudo, service accounts, containers).
QString CurrentUserId()
{
    QString env = QProcessEnvironment::systemEnvironment().value(
#ifdef Q_OS_WIN
        QStringLiteral("USERNAME")
#else
        QStringLiteral("USER")
#endif
    );
    if (!env.isEmpty())
    {
        return env;
    }
#ifdef Q_OS_WIN
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

/// Path for the `probe -> listen` takeover lock. `TempLocation`
/// over `RuntimeLocation` to avoid Linux's per-login-session sticky.
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
        // Disconnect first so an in-flight connection cannot race
        // the teardown below.
        disconnect(mServer.get(), nullptr, this, nullptr);
        const auto sockets = mConnections.keys();
        for (QLocalSocket *socket : sockets)
        {
            DropConnection(socket);
        }
        // `removeServer` unlinks the socket file on Linux so a
        // fast relaunch doesn't see a dead peer.
        mServer->close();
        QLocalServer::removeServer(mSocketName);
    }
}

void SingleInstanceGuard::SetSocketNameForTest(const QString &name)
{
    // Must be called before `TryAcquire`; otherwise the live
    // server stays bound to the old name.
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
        // `--new-instance` bypass: run uncoordinated without
        // touching the canonical primary's socket.
        return true;
    }

    // Pre-truncate to the count cap; `truncatedCount` flows to the
    // primary so it surfaces a "N files dropped" hint.
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

    // Serialise up front so we can enforce `MAX_PAYLOAD_BYTES`
    // before any round-trip. Long paths can blow past the byte
    // cap with 256 entries, so pop tail entries on overrun.
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
    // Empty forward is fine (secondary asks for an empty window);
    // only reject if tail-trim emptied the input but bytes still
    // overrun.
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
        // `flush` / `waitForBytesWritten` may return false when
        // the OS already drained (common on Windows named pipes).
        probe.flush();
        probe.waitForBytesWritten(WRITE_TIMEOUT_MS);
        // Wait for the primary's ack via disconnect; only count it
        // a stall when still connected with bytes pending.
        const bool ack = probe.waitForDisconnected(WRITE_TIMEOUT_MS);
        if (!ack && probe.state() == QLocalSocket::ConnectedState && probe.bytesToWrite() > 0)
        {
            logapp::LogWarning() << "SingleInstanceGuard: forward stalled with" << probe.bytesToWrite()
                                 << "bytes still pending; error:" << probe.error() << probe.errorString();
            return false;
        }
        return true;
    };

    // Step 1: try to forward to an existing primary. Failures
    // fall through to step 2 so the user always gets a window.
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
        }
    }

    // Step 2: become the primary. Serialise the racy `removeServer
    // + listen` pair via a cross-process `QLockFile`.
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
        // Retry failed; run uncoordinated rather than hang.
        return true;
    }

    QLocalServer::removeServer(mSocketName);
    mServer = std::make_unique<QLocalServer>();
    mServer->setSocketOptions(QLocalServer::UserAccessOption);
    // Wire `newConnection` before `listen()`; some platforms
    // surface queued connections synchronously inside `listen()`.
    connect(mServer.get(), &QLocalServer::newConnection, this, &SingleInstanceGuard::HandleNewConnection);
    if (!mServer->listen(mSocketName))
    {
        const QString listenError = mServer->errorString();
        // Another process bound between `removeServer` and
        // `listen`. Try one more forward before giving up.
        QLocalSocket retry;
        retry.connectToServer(mSocketName);
        if (retry.waitForConnected(CONNECT_TIMEOUT_MS))
        {
            const bool forwarded = payloadFits && forwardTo(retry);
            mServer.reset();
            takeover.unlock();
            return !forwarded;
        }
        // Nothing listening either. Most often this is a too-long
        // socket path (macOS: sun_path is 104 bytes, Linux: 108).
        // Log loud so the user-visible "running uncoordinated"
        // mode does not get blamed on something else.
        logapp::LogWarning() << "SingleInstanceGuard: listen on" << mSocketName << "failed:" << listenError
                             << "-- running uncoordinated (no primary).";
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
        // Timer is parented to the socket; QObject ownership
        // takes care of deletion.
    }
    mConnections.erase(it);
}

void SingleInstanceGuard::HandleNewConnection()
{
    while (QLocalSocket *socket = mServer ? mServer->nextPendingConnection() : nullptr)
    {
        ConnState &state = mConnections[socket];
        // Idle watchdog: restarted on every `readyRead`, so "idle"
        // means "no activity for N ms".
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

        // Re-parse from byte 0 on each `readyRead`. `QDataStream`
        // is non-destructive; total bounded by `MAX_PAYLOAD_BYTES`.
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
            if (it->idleTimer != nullptr)
            {
                it->idleTimer->start();
            }

            // Magic + version peek so a junk peer never makes us
            // allocate a large QStringList.
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
            // Defence in depth: enforce the count cap post-decode
            // in case a peer packed many short files past the limit.
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

        // Windows named pipes may already have data buffered when
        // `newConnection` fires; drain it now since `readyRead`
        // won't replay it. Never `waitForReadyRead` -- a silent
        // peer would freeze the GUI thread.
        if (socket->bytesAvailable() > 0)
        {
            tryDecode();
        }
    }
}

QString SingleInstanceGuard::DefaultSocketName()
{
    // Salt with `applicationName` so nightly / stable / sandboxed
    // installs get independent primaries.
    const QString appId = QCoreApplication::applicationName();
    Q_ASSERT_X(
        !appId.isEmpty(),
        "SingleInstanceGuard::DefaultSocketName",
        "QCoreApplication::applicationName() is empty; call setApplicationName before constructing the guard"
    );

    // Per-user salt so two users on the same host get independent
    // primaries.
    const QString user = CurrentUserId();
    const QByteArray salt = (user + QLatin1Char('|') + appId).toUtf8();

    const QByteArray digest = QCryptographicHash::hash(salt, QCryptographicHash::Sha1).toHex().left(16);
    return QStringLiteral("structured-log-viewer.") + QString::fromLatin1(digest);
}
