#pragma once

#include "anchor_manager.hpp"

#include <QDockWidget>
#include <QPointer>

class LogModel;
class ThemeControl;
class QCloseEvent;
class QTreeWidget;
class QTreeWidgetItem;
class QPushButton;

/// Dockable list of every anchored row. Each entry shows a colour
/// swatch, the row's `lineId`, and the source filename (when known),
/// plus an inline-editable one-line note. Double-click on the anchor
/// column jumps to the row via `jumpToAnchorRequested`; double-click
/// or `F2` on the note column starts inline editing. Right-click
/// offers Jump / Edit note / Remove. A header button clears everything.
///
/// Stays in sync with `AnchorManager` and `ThemeControl` through
/// signals. Refresh work is gated on visibility so a buried dock pays
/// nothing.
///
/// All three collaborators are borrowed (non-owning) and must outlive
/// the dock.
class AnchorsDock : public QDockWidget
{
    Q_OBJECT

public:
    AnchorsDock(AnchorManager *anchors, LogModel *model, ThemeControl *theme, QWidget *parent = nullptr);

    /// Refresh from `AnchorManager::Entries()` if visible; no-op otherwise.
    void Refresh();

    /// True when the dock should actually rebuild on a signal. Offscreen
    /// QPA fixtures never get a `visibilityChanged` and default to false.
    [[nodiscard]] bool IsVisibleForRefresh() const noexcept;

signals:
    /// User asked to navigate to source-model row @p sourceRow.
    /// Argument is -1 when the anchor key has no live row.
    void jumpToAnchorRequested(int sourceRow);

    /// Emitted on genuine user dismissal (X button, system close).
    /// Distinct from `visibilityChanged(false)`, which also fires on
    /// tab inactivation in a tabified group.
    void closed();

protected:
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

    /// Unconditional refresh for offscreen-QPA tests that can't drive
    /// real `visibilityChanged` events.
    void RefreshForTest()
    {
        RefreshAlways();
    }

    /// Trigger the inline note editor on the currently focused item,
    /// bypassing the F2 shortcut path (which needs an event loop).
    void BeginEditNoteForTest();
#endif

private:
    /// Resolve @p item's key back to a `LogModel` source-row index, or
    /// -1 if the anchor outlived its row.
    [[nodiscard]] int SourceRowForItem(const QTreeWidgetItem *item) const;

    /// Rebuild from `AnchorManager::Entries()` unconditionally.
    void RefreshAlways();

    void OnItemActivated(QTreeWidgetItem *item, int column);
    void OnItemChanged(QTreeWidgetItem *item, int column);
    void OnContextMenuRequested(const QPoint &pos);
    void OnClearAllClicked();

    QPointer<AnchorManager> mAnchors;
    QPointer<LogModel> mModel;
    QPointer<ThemeControl> mTheme;

    QTreeWidget *mTree = nullptr;
    QPushButton *mClearAllButton = nullptr;

    /// Tracks `visibilityChanged` so a buried tabified dock also skips
    /// signal-driven refreshes. Starts false because the dock is added
    /// hidden; flipped by the first `visibilityChanged(true)`.
    bool mPerceivedVisible = false;

    /// Suppresses the `itemChanged -> SetAnchorNote` round trip during
    /// `RefreshAlways`, which mutates item text to rebuild the list.
    /// Guarded with an int (not bool) because Qt can nest signals if
    /// a delegate close-editor fires while a refresh is still walking.
    int mSuppressItemChanged = 0;
};
