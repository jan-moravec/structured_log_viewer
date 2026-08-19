#pragma once

#include "scoped_connections.hpp"

#include <QDockWidget>
#include <QPersistentModelIndex>
#include <QPointer>

class AnchorManager;
class LogModel;
class LogSession;
class RecordDetailWidget;
class QCloseEvent;
struct SessionBindContext;

/**
 * @brief Dockable details view pinned to one source-model row.
 *
 * A `QPersistentModelIndex` follows surviving rows through insertions
 * and removals. Eviction invalidates the pin and displays a dedicated
 * placeholder. Expensive refreshes are skipped while the dock is not
 * perceptually visible.
 */
class RecordDetailDock : public QDockWidget
{
    Q_OBJECT

public:
    /**
     * @brief Constructs a record-detail dock for borrowed sources.
     * @param model Log model to observe.
     * @param anchors Anchor manager to observe, or `nullptr`.
     * @param parent Parent widget.
     */
    RecordDetailDock(LogModel *model, AnchorManager *anchors = nullptr, QWidget *parent = nullptr);

    /**
     * @brief Pins and displays a source row.
     * @param sourceRow Source-model row; invalid rows clear the view.
     */
    void ShowSourceRow(int sourceRow);

    /** @brief Clears the pin and shows the default placeholder. */
    void Clear();

    /**
     * @brief Returns the current pinned source row.
     * @return Source row, or -1 when no live row is pinned.
     */
    [[nodiscard]] int CurrentSourceRow() const noexcept;

    /**
     * @brief Returns the hosted detail widget.
     * @return A borrowed pointer owned by the dock.
     */
    [[nodiscard]] RecordDetailWidget *Widget() const noexcept
    {
        return mWidget;
    }

    /**
     * @brief Binds the detail dock to a session context.
     * @param context Incoming session, model, and anchor sources.
     *
     * A changed binding saves the outgoing pin, invalidates the
     * persistent index before replacing its model, reconnects source
     * subscriptions, and restores the incoming pin. An identical
     * non-null binding is a no-op. An unbound context detaches sources
     * and shows the default placeholder. No ownership is transferred.
     */
    void Bind(const SessionBindContext &context);

    /** @brief Saves current pin state and releases borrowed session sources. */
    void Unbind();

    /**
     * @brief Returns the currently bound session for tests.
     * @return The borrowed session, or `nullptr` when unbound.
     */
    [[nodiscard]] LogSession *boundSessionForTest() const noexcept;

    /**
     * @brief Reports whether model-driven refresh work should run.
     * @return True when the dock is perceptually visible.
     */
    [[nodiscard]] bool IsVisibleForRefresh() const noexcept;

signals:
    /**
     * @brief Requests opening the current record in another window.
     * @param sourceRow Current source row, or -1 when no row is pinned.
     */
    void openInNewWindowRequested(int sourceRow);

    /** @brief Emitted when the user closes the dock. */
    void closed();

protected:
    /**
     * @brief Emits `closed()` after an accepted close.
     * @param event Close event.
     */
    void closeEvent(QCloseEvent *event) override;

#ifdef LOGAPP_BUILD_TESTING
public:
    /**
     * @brief Returns the number of model refreshes for tests.
     * @return Refresh invocation count.
     */
    [[nodiscard]] int RefreshCountForTest() const noexcept
    {
        return mRefreshCount;
    }
#endif

private:
    void RefreshFromModel();
    void OnOpenInNewWindowRequested();

    /** @brief Shows the evicted-record placeholder while retaining pin history. */
    void ShowEvictedPlaceholder();

    /** @brief Installs subscriptions for the current source aliases. */
    void InstallSourceSubscriptions();

    /** @brief Saves the live pin and stable row identity into the bound session. */
    void SaveStateIntoBoundSession();

    /**
     * @brief Restores a session's saved pin or placeholder state.
     * @param session Session whose state should be restored, or `nullptr`.
     */
    void RestoreStateFromSession(LogSession *session);

    QPointer<LogModel> mModel;
    QPointer<AnchorManager> mAnchors;
    RecordDetailWidget *mWidget = nullptr;
    // Persistent source-model pin; invalid when absent or evicted.
    QPersistentModelIndex mCurrentSourceIndex;
    // Distinguishes a never-pinned record from an evicted pin.
    bool mEverPinned = false;
    // Tracks whether updates should refresh the visible dock.
    bool mPerceivedVisible = true;

    // Subscriptions owned by the current source binding.
    ScopedConnections mSourceConnections;

    // Session whose record-detail state is currently mirrored.
    QPointer<LogSession> mBoundSession;
#ifdef LOGAPP_BUILD_TESTING
    int mRefreshCount = 0;
#endif
};
