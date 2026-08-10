#pragma once

#include <QHash>
#include <QLocalServer>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <memory>

class QLocalSocket;
class QTimer;

/// Cross-process coordinator that funnels every launch to the first
/// running instance. The first process to `listen()` becomes the
/// primary; subsequent launches forward their CLI files to it and
/// then exit, so the user sees a single application.
///
/// Bypass: pass `--new-instance` (or set `LOGAPP_NEW_INSTANCE=1`) to
/// run uncoordinated alongside the canonical primary.
class SingleInstanceGuard : public QObject
{
    Q_OBJECT
    Q_DISABLE_COPY_MOVE(SingleInstanceGuard)
public:
    /// Per-launch flags forwarded by a secondary process.
    /// Wire values map directly to the v3+ `quint32` field. Never
    /// reuse a bit; incompatible changes require a version bump.
    enum class LaunchFlags : quint32
    {
        None = 0,
        /// Disable rotation-sibling detection in the new window.
        DisableRotationHistory = 1u << 0,
    };
    Q_ENUM(LaunchFlags)
    Q_DECLARE_FLAGS(LaunchFlagsBitmask, LaunchFlags)
    Q_FLAG(LaunchFlagsBitmask)

    explicit SingleInstanceGuard(QObject *parent = nullptr);
    ~SingleInstanceGuard() override;

    /// Try to take the primary role.
    ///
    /// - No existing primary -> start a `QLocalServer` and return
    ///   `true`. Future secondary connections fire
    ///   `openWindowRequested`.
    /// - Primary already listening -> forward @p forwardFiles (an
    ///   empty list still asks the primary to raise a new window)
    ///   and return `false`. The caller should exit immediately.
    /// - @p allowNewInstance is `true` -> skip both paths and run
    ///   uncoordinated. We deliberately do NOT `removeServer` +
    ///   `listen` here, which on Linux would unlink the canonical
    ///   socket file and silently zombie the existing primary.
    ///
    /// @p launchFlags is sent only when forwarding to a primary.
    [[nodiscard]] bool TryAcquire(
        const QStringList &forwardFiles, bool allowNewInstance, LaunchFlagsBitmask launchFlags = LaunchFlags::None
    );

    /// Socket name used by `QLocalServer`. Exposed for tests.
    [[nodiscard]] QString SocketName() const noexcept
    {
        return mSocketName;
    }

    /// Override the socket name. Must be called before `TryAcquire`.
    /// Tests use this to isolate cases from each other and from the
    /// user's real running instance.
    void SetSocketNameForTest(const QString &name);

signals:
    /// Fired on the primary when a secondary launch arrives. The
    /// primary should spawn a new `MainWindow` and open @p files
    /// (an empty list still means "open a new empty window").
    /// @p truncatedCount is the number of additional files the
    /// secondary held but did not send because it hit
    /// `MAX_FORWARDED_FILES`; the primary uses this to surface a
    /// status-bar warning so the user knows part of their input
    /// was dropped on the wire.
    /// @p launchFlags is `None` for wire versions before 3.
    void openWindowRequested(const QStringList &files, int truncatedCount, LaunchFlagsBitmask launchFlags);

private:
    /// Default per-user socket name. Includes the platform username
    /// so two users on the same host get independent primaries.
    [[nodiscard]] static QString DefaultSocketName();

    void HandleNewConnection();

    /// Per-connection state held on the primary side.
    struct ConnState
    {
        QByteArray buffer;
        QTimer *idleTimer = nullptr;
        /// Prevents re-decoding while asynchronous disconnect is pending.
        bool frameConsumed = false;
    };

    /// Forget a disconnected socket. Called from the `disconnected`
    /// slot and from the destructor before tearing the server down.
    void DropConnection(QLocalSocket *socket);

    QString mSocketName;
    std::unique_ptr<QLocalServer> mServer;
    QHash<QLocalSocket *, ConnState> mConnections;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(SingleInstanceGuard::LaunchFlagsBitmask)

// `Q_FLAG` registers meta-object data; this also enables
// `QVariant::value<LaunchFlagsBitmask>()`.
Q_DECLARE_METATYPE(SingleInstanceGuard::LaunchFlagsBitmask)
