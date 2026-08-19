#pragma once

#include "anchor_manager.hpp"
#include "scoped_connections.hpp"

#include <QDockWidget>
#include <QPointer>

class LogModel;
class LogSession;
class ThemeControl;
class QCloseEvent;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;
struct SessionBindContext;

/**
 * @brief Dockable list of anchored rows and editable anchor notes.
 *
 * The dock borrows its session collaborators, tracks their signals,
 * and defers refresh work while hidden.
 */
class AnchorsDock : public QDockWidget
{
    Q_OBJECT

public:
    AnchorsDock(AnchorManager *anchors, LogModel *model, ThemeControl *theme, QWidget *parent = nullptr);

    /** @brief Refreshes the anchor list when the dock is visible. */
    void Refresh();

    /**
     * @brief Binds the dock to a session context.
     * @param context Borrowed session, model, anchor manager, and theme aliases.
     *
     * A changed binding saves the outgoing current anchor, closes
     * editors, disconnects subscriptions, clears the tree, installs
     * the new aliases, and restores the incoming current anchor when
     * the tree is populated. An identical non-null binding is a no-op.
     * An unbound context produces the same visible state as `Unbind()`.
     * No ownership is transferred.
     */
    void Bind(const SessionBindContext &context);

    /**
     * @brief Releases borrowed session sources and clears the tree.
     *
     * The window-scoped theme alias is retained.
     */
    void Unbind();

    /**
     * @brief Returns the currently bound session for tests.
     * @return The borrowed session, or `nullptr` when unbound.
     */
    [[nodiscard]] LogSession *boundSessionForTest() const noexcept;

    /**
     * @brief Returns the current anchor manager for tests.
     * @return The borrowed anchor manager, or `nullptr` when unbound.
     */
    [[nodiscard]] AnchorManager *anchorsForTest() const noexcept
    {
        return mAnchors.data();
    }

    /**
     * @brief Reports whether signal-driven refresh work should run.
     * @return True when the dock is explicitly and perceptually visible.
     */
    [[nodiscard]] bool IsVisibleForRefresh() const noexcept;

    /** @brief Opens the current anchor's inline note editor, if any. */
    void BeginEditingCurrentNote();

    /**
     * @brief Gives plain F2 to the focused note cell.
     * @param watched Object receiving the event.
     * @param event Event being filtered.
     * @return True when the shortcut override is consumed.
     */
    bool eventFilter(QObject *watched, QEvent *event) override;

signals:
    /**
     * @brief Requests navigation to an anchored source row.
     * @param sourceRow Source-model row, or -1 when the anchor has no live row.
     */
    void jumpToAnchorRequested(int sourceRow);

    /** @brief Emitted when the user closes the dock. */
    void closed();

protected:
    /**
     * @brief Handles dock closure and emits `closed()` when accepted.
     * @param event Close event.
     */
    void closeEvent(QCloseEvent *event) override;

#ifdef LOGAPP_BUILD_TESTING
public:
    [[nodiscard]] QTreeWidget *TreeForTest() const noexcept
    {
        return mTree;
    }

    [[nodiscard]] QPushButton *ClearAllButtonForTest() const noexcept
    {
        return mClearAllButton;
    }

    /** @brief Refreshes unconditionally for tests. */
    void RefreshForTest()
    {
        RefreshAlways();
    }

    /** @brief Opens the current note editor for tests. */
    void BeginEditNoteForTest()
    {
        BeginEditingCurrentNote();
    }
#endif

private:
    /**
     * @brief Resolves an item's anchor key to a source row.
     * @param item Tree item to resolve.
     * @return Source-model row, or -1 when unavailable.
     */
    [[nodiscard]] int SourceRowForItem(const QTreeWidgetItem *item) const;

    /** @brief Rebuilds the tree without applying the visibility gate. */
    void RefreshAlways();

    void OnItemActivated(QTreeWidgetItem *item, int column);
    void OnItemChanged(QTreeWidgetItem *item, int column);
    void OnContextMenuRequested(const QPoint &pos);
    void OnClearAllClicked();

    /**
     * @brief Updates one anchor entry without replacing its note editor.
     * @param key Anchor key that changed.
     */
    void OnAnchorChanged(const AnchorManager::Key &key);

    /**
     * @brief Updates one anchor note and its tooltip.
     * @param key Anchor key whose note changed.
     */
    void OnAnchorNoteChanged(const AnchorManager::Key &key);

    QPointer<AnchorManager> mAnchors;
    QPointer<LogModel> mModel;
    QPointer<ThemeControl> mTheme;

    QTreeWidget *mTree = nullptr;
    QPushButton *mClearAllButton = nullptr;

    // Tracks visibility so hidden tabified docks skip refreshes.
    bool mPerceivedVisible = false;

    // Suppresses item-change feedback while refreshing tree text.
    int mSuppressItemChanged = 0;

    // Subscriptions owned by the current session binding.
    ScopedConnections mSessionConnections;

    // Currently bound session, or null while unbound.
    QPointer<LogSession> mBoundSession;

    /** @brief Installs subscriptions for the current source aliases. */
    void InstallSessionSubscriptions();

    /** @brief Closes inline editors without committing into an outgoing session. */
    void CloseInPlaceEditors();
};
