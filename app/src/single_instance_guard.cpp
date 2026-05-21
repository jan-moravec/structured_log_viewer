#include "single_instance_guard.hpp"

#include <QByteArray>
#include <QCryptographicHash>
#include <QDataStream>
#include <QLocalSocket>
#include <QProcessEnvironment>
#include <QString>
#include <QVariant>

namespace
{
/// Magic prefix on the wire so a junk client cannot hand us garbage
/// strings and have us treat them as file paths.
constexpr char WIRE_MAGIC[] = "STRUCTLOGV1";

/// Connection / write timeouts. Tight on purpose: a secondary that
/// cannot reach the primary within a second should fall back to
/// becoming its own primary rather than block the user's launch.
constexpr int CONNECT_TIMEOUT_MS = 1000;
constexpr int WRITE_TIMEOUT_MS = 1000;

/// Hard cap on inbound payload size so a malformed / hostile peer
/// cannot OOM the primary by sending a giant stream. 1 MiB is far
/// above what a CLI invocation would ever carry.
constexpr qint64 MAX_PAYLOAD_BYTES = 1024 * 1024;
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
    Q_ASSERT(mServer == nullptr);
    mSocketName = name;
}

bool SingleInstanceGuard::TryAcquire(const QStringList &forwardFiles, bool allowNewInstance)
{
    auto forwardTo = [&](QLocalSocket &probe) {
        QByteArray payload;
        QDataStream stream(&payload, QIODevice::WriteOnly);
        stream.setVersion(QDataStream::Qt_6_0);
        stream << QByteArray(WIRE_MAGIC);
        stream << forwardFiles;
        probe.write(payload);
        probe.flush();
        probe.waitForBytesWritten(WRITE_TIMEOUT_MS);
        // Wait for the primary to ack by tearing the connection
        // down on its end. Without this the secondary process
        // may exit before the primary drains the OS-level pipe
        // buffer (observed on Windows named pipes).
        probe.waitForDisconnected(WRITE_TIMEOUT_MS);
    };

    if (!allowNewInstance)
    {
        // Step 1: try to connect as a secondary. If the connect
        // succeeds, the primary is up and we forward.
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
    mServer = std::make_unique<QLocalServer>(this);
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
        // window. The lock-file backstop in Part 5b prevents the
        // resulting recents-index corruption.
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
        auto tryDecode = [this, socket]() {
            QByteArray buffer = socket->property("structlog_buf").toByteArray();
            buffer.append(socket->readAll());
            if (buffer.size() > MAX_PAYLOAD_BYTES)
            {
                socket->disconnectFromServer();
                return;
            }

            QDataStream peek(&buffer, QIODevice::ReadOnly);
            peek.setVersion(QDataStream::Qt_6_0);
            QByteArray magic;
            peek >> magic;
            QStringList files;
            peek >> files;
            if (peek.status() == QDataStream::Ok)
            {
                if (magic == QByteArray(WIRE_MAGIC))
                {
                    emit openWindowRequested(files);
                }
                socket->disconnectFromServer();
                return;
            }
            socket->setProperty("structlog_buf", buffer);
        };

        connect(socket, &QLocalSocket::readyRead, this, tryDecode);

        // On Windows named pipes the data may already have arrived
        // by the time `newConnection` fires; the readyRead signal
        // does not fire retroactively for already-buffered bytes
        // when wired this late. Pump synchronously once so we drain
        // anything that was waiting.
        if (socket->bytesAvailable() > 0 || socket->waitForReadyRead(WRITE_TIMEOUT_MS))
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

    // Hash to a fixed length so the resulting name is bounded
    // regardless of username; prefix with the app id so multiple
    // products on the same host coexist.
    const QByteArray digest =
        QCryptographicHash::hash(user.toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    return QStringLiteral("structured-log-viewer.") + QString::fromLatin1(digest);
}
