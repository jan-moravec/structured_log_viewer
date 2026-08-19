#pragma once

#include "log_model.hpp"

class AnchorManager;
class HighlightRuleSet;
class LogFilterModel;
class LogSession;
class LogSessionView;
class LogTableView;
class MainWindow;
class RowOrderProxyModel;

/**
 * @brief Non-owning handles for one hosted session's operation pipeline.
 *
 * Completions resolve a `SessionInstanceId` in the window's hosted-tab
 * registry and then use this target so they never read the active-tab
 * aliases.
 */
struct SessionOperationTarget
{
    LogSession *session = nullptr;
    LogSessionView *view = nullptr;
    LogModel *model = nullptr;
    RowOrderProxyModel *rowOrder = nullptr;
    LogFilterModel *filter = nullptr;
    AnchorManager *anchors = nullptr;
    HighlightRuleSet *highlights = nullptr;
    LogTableView *table = nullptr;
    bool isActive = false;

    /**
     * @brief Tests whether the target names a hosted session with a model.
     *
     * @return `true` when `session` and `model` are non-null.
     */
    [[nodiscard]] bool isValid() const noexcept
    {
        return session != nullptr && model != nullptr;
    }
};

/**
 * @brief Routes ingest, decompression, and export work to a hosted session.
 *
 * Worker objects live on `LogSession`. The window remains the host for
 * chrome, dialogs, and menus. Completions resolve hosted identity first
 * and then update only that session.
 */
class SessionOperationController
{
public:
    /**
     * @brief Constructs a controller bound to one window.
     *
     * @param window Window that hosts tabs and chrome.
     */
    explicit SessionOperationController(MainWindow &window) noexcept;

    /**
     * @brief Builds origin handles for a hosted session.
     *
     * @param origin Candidate session, or `nullptr`.
     * @return A valid target when `origin` is still hosted; an empty target otherwise.
     */
    [[nodiscard]] SessionOperationTarget TargetFor(LogSession *origin) const noexcept;

    /**
     * @brief Completes streaming for a hosted origin session.
     *
     * @param origin Session whose parse worker finished.
     * @param result Terminal streaming result.
     */
    void CompleteStreaming(LogSession *origin, StreamingResult result);

    /**
     * @brief Completes decompression for a hosted origin session.
     *
     * @param origin Session whose decompression watcher finished.
     */
    void CompleteDecompression(LogSession *origin);

    /**
     * @brief Completes export for a hosted origin session.
     *
     * @param origin Session whose export watcher finished.
     */
    void CompleteExport(LogSession *origin);

    /**
     * @brief Writes a restorable snapshot for one hosted session.
     *
     * Does not activate the session or rebind shared docks.
     *
     * @param origin Session to persist, or `nullptr`.
     * @param publishOpenWindow Whether to publish the UUID for launch restore.
     * @return `true` when a snapshot was written or none was required.
     */
    [[nodiscard]] bool SaveSnapshot(LogSession *origin, bool publishOpenWindow);

    /**
     * @brief Writes restorable snapshots for every hosted session.
     *
     * Walks hosted tabs in order without changing the selected tab.
     *
     * @param publishOpenWindow Whether to publish each UUID for launch restore.
     */
    void SaveAllHostedSnapshots(bool publishOpenWindow);

private:
    /** @brief Non-owning host window; set by the constructor and never null. */
    MainWindow *mWindow = nullptr;
};
