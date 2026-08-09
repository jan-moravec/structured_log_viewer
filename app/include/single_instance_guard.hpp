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
    /// Bitmask of per-launch flags that a secondary process forwards
    /// to the primary. Kept small on purpose: only flags whose scope
    /// is "for this launch" belong here. Everything else lives in
    /// `QSettings` or `LogConfiguration` where the primary can
    /// consult it directly.
    ///
    /// Wire compatibility: bits map 1:1 onto the `quint32` field
    /// appended after `truncatedCount` in wire version 3+. Reorder
    /// only with a version bump; add new bits at the end.
    enum class LaunchFlags : quint32
    {
        None = 0,
        /// `--no-rotation-history`: suppresses rotation-sibling
        /// auto-detection for the resulting window. Without this
        /// bit on the wire, the secondary's flag would be dropped
        /// on the way to the primary and the CLI would silently
        /// no-op whenever a primary already exists.
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
    /// @p launchFlags accumulates per-launch flags that the primary
    /// should apply to the freshly-spawned peer window. Only used
    /// on the forward path (unused when @p allowNewInstance is set
    /// or when this process becomes the primary).
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
    /// @p launchFlags carries the per-launch bitmask the secondary
    /// sent (e.g. `--no-rotation-history`). Always
    /// `LaunchFlags::None` for wire versions older than 3, so a
    /// stale-binary secondary still funnels through cleanly.
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
    };

    /// Forget a disconnected socket. Called from the `disconnected`
    /// slot and from the destructor before tearing the server down.
    void DropConnection(QLocalSocket *socket);

    QString mSocketName;
    std::unique_ptr<QLocalServer> mServer;
    QHash<QLocalSocket *, ConnState> mConnections;
};

Q_DECLARE_OPERATORS_FOR_FLAGS(SingleInstanceGuard::LaunchFlagsBitmask)

// Register the bitmask type with `QMetaType` so `QVariant` (used by
// `QSignalSpy` when it captures signal arguments) can round-trip it
// via `value<LaunchFlagsBitmask>()`. `Q_FLAG` inside the Q_OBJECT
// registers the flags with the meta-*object*, but that is not the
// same as the meta-*type* system `QVariant::value<T>()` consults.
Q_DECLARE_METATYPE(SingleInstanceGuard::LaunchFlagsBitmask)
