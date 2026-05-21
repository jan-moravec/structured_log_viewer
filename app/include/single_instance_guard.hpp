#pragma once

#include <QLocalServer>
#include <QObject>
#include <QString>
#include <QStringList>

#include <memory>

/// Cross-process coordinator that funnels every launch of the
/// application to the *first* running instance (mirrors the VS Code
/// behaviour the user picked over multi-process). The first process to
/// `listen()` becomes the primary; subsequent launches detect the
/// existing socket, hand the parsed CLI files over to the primary, and
/// then exit so the OS user sees a single application.
///
/// Bypass: pass `--new-instance` (or set `LOGAPP_NEW_INSTANCE=1`) and
/// the would-be secondary refuses to forward, becomes its own
/// primary, and the user ends up with two processes (useful for
/// debugging or for explicitly running side-by-side processes).
class SingleInstanceGuard : public QObject
{
    Q_OBJECT
public:
    explicit SingleInstanceGuard(QObject *parent = nullptr);
    ~SingleInstanceGuard() override;

    /// Try to take the primary role. Behaviour matches VS Code's
    /// "single instance, open new window for second launch":
    ///
    /// - If no existing primary exists (no socket / stale socket),
    ///   start a new `QLocalServer` and return `true`. The current
    ///   process is the primary; subsequent secondary connections
    ///   will fire `openWindowRequested`.
    /// - If a primary is already listening, send @p forwardFiles to
    ///   it (an empty list still triggers a "raise a new window"
    ///   request) and return `false`. The caller is expected to exit
    ///   the application immediately.
    /// - If @p allowNewInstance is `true`, skip both the forward
    ///   and the listen path. The process runs as a fully
    ///   uncoordinated primary: plain launches still reach the
    ///   canonical primary's socket, and this process will not see
    ///   forwarded files from future secondaries. Going through
    ///   `removeServer` + `listen` would unlink the canonical socket
    ///   file on Linux and silently zombie the existing primary.
    [[nodiscard]] bool TryAcquire(const QStringList &forwardFiles, bool allowNewInstance);

    /// Server socket name used by `QLocalServer`. Exposed for tests
    /// (the in-process spy connects directly to this socket).
    [[nodiscard]] QString SocketName() const noexcept
    {
        return mSocketName;
    }

    /// Override the socket name used by `TryAcquire`. Must be called
    /// before `TryAcquire`. Tests use this to isolate cases from
    /// each other and from the user's real running instance.
    void SetSocketNameForTest(const QString &name);

signals:
    /// Fired on the primary process when a secondary launch arrives.
    /// @p files is the (possibly empty) CLI file list that the
    /// secondary forwarded; the primary should respond by spawning a
    /// new `MainWindow` and opening those files. An empty list still
    /// means "open a new empty window", per VS Code's UX.
    void openWindowRequested(const QStringList &files);

private:
    /// Compute the default per-user socket name. Includes the
    /// platform's username so two users on the same host get
    /// independent primaries.
    [[nodiscard]] static QString DefaultSocketName();

    /// Handle a freshly-connected secondary on the primary side.
    void HandleNewConnection();

    QString mSocketName;
    std::unique_ptr<QLocalServer> mServer;
};
