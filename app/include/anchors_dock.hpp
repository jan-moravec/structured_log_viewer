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
/// Stays in sync with `AnchorManager` and `ThemeControl` via signals.
/// Refresh work is gated on visibility so a buried dock pays nothing.
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

    /// Open the inline note editor on the current tree item's note
    /// column. No-op when the tree has no current item. Called by
    /// `MainWindow::EditAnchorNoteOnCurrentRow` to redirect F4 to
    /// the inline editor when focus is in the dock, matching the
    /// dock's own F2 / double-click gesture.
    void BeginEditingCurrentNote();

signals:
    /// User asked to navigate to source-model row @p sourceRow.
    /// Argument is -1 when the anchor key has no live row.
    void jumpToAnchorRequested(int sourceRow);

    /// Emitted on genuine user dismissal (X button, system close).
    /// Distinct from `visibilityChanged(false)`, which also fires on
    /// tab inactivation in a tabified group.
    void closed();

    // No runtime-only-note signal here: `AnchorManager::Entries()`
    // filters those out, so they never appear in the tree. The
    // "session-only" warning lives on the F4 / row-menu path.

protected:
    void closeEvent(QCloseEvent *event) override;

    /// Vetoes the window-scope `F2` shortcut when the tree's note
    /// column has focus, so the inline editor opens instead of
    /// "Jump to next anchor" firing. Everything else propagates
    /// normally.
    bool eventFilter(QObject *watched, QEvent *event) override;

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

    /// Thin forwarder to `BeginEditingCurrentNote` for tests that
    /// can't drive the F2 shortcut through a real event loop.
    void BeginEditNoteForTest()
    {
        BeginEditingCurrentNote();
    }
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

    /// Surgical single-key refresh for `anchorChanged`. Handles add
    /// (insert at the sorted position), remove (delete the item),
    /// and update (refresh swatch + label + tooltip + key data) in
    /// place. The note cell is intentionally left alone -- notes
    /// flow through `OnAnchorNoteChanged` so a colour flip elsewhere
    /// doesn't clobber an in-flight inline note edit.
    void OnAnchorChanged(const AnchorManager::Key &key);

    /// Surgical note-only refresh: rewrite the matching item's note
    /// cell and tooltip in place. Falls back to `RefreshAlways`
    /// when the key isn't currently displayed (dock was hidden, or
    /// a race with a bulk reset dropped the item).
    void OnAnchorNoteChanged(const AnchorManager::Key &key);

    QPointer<AnchorManager> mAnchors;
    QPointer<LogModel> mModel;
    QPointer<ThemeControl> mTheme;

    QTreeWidget *mTree = nullptr;
    QPushButton *mClearAllButton = nullptr;

    /// Tracks `visibilityChanged` so a buried tabified dock also skips
    /// signal-driven refreshes. Starts false because the dock is added
    /// hidden; flipped by the first `visibilityChanged(true)`.
    bool mPerceivedVisible = false;

    /// Suppresses the `itemChanged -> SetAnchorNote` round trip
    /// while `RefreshAlways` / `OnAnchor*Changed` are mutating item
    /// text. A counter (not a bool) because Qt can nest the
    /// signals if a delegate close-editor fires mid-refresh.
    int mSuppressItemChanged = 0;
};
