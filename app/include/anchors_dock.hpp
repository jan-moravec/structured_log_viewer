#pragma once

#include "anchor_manager.hpp"

#include <QDockWidget>
#include <QPointer>

class LogModel;
class ThemeControl;
class QListWidget;
class QListWidgetItem;
class QPushButton;

/// Dockable list of every anchored row in the active session. Each
/// entry shows the anchor's colour swatch, the row's stable lineId,
/// and (when the anchor carries a non-empty locator) the source
/// path's filename. Double-click emits
/// `jumpToAnchorRequested(sourceRow)`; right-click offers
/// "Jump to anchor" / "Remove anchor". A header button wipes every
/// anchor via the owning `AnchorManager`.
///
/// The dock listens to `AnchorManager::anchorChanged` and
/// `anchorsReset` so the list stays in lockstep with the model
/// without polling, and to `ThemeControl::themeChanged` so swatch
/// icons re-render with the new theme's `anchorPalette`. Refresh
/// work is gated on `IsVisibleForRefresh()` so a hidden /
/// buried-tab dock does no list rebuilding.
///
/// Ownership: the `AnchorManager`, `LogModel`, and `ThemeControl`
/// are borrowed; all three must outlive the dock. The dock parents
/// itself to its owning `MainWindow` and is destroyed with it.
class AnchorsDock : public QDockWidget
{
    Q_OBJECT

public:
    AnchorsDock(AnchorManager *anchors, LogModel *model, ThemeControl *theme, QWidget *parent = nullptr);

    /// Refresh from `AnchorManager::Entries()` if the dock is
    /// actually visible -- buried docks short-circuit immediately
    /// so signal-driven refreshes pay nothing when the user can't
    /// see them. `RefreshForTest` exposes the unconditional path
    /// for the offscreen QPA fixtures that can't drive
    /// `visibilityChanged`.
    void Refresh();

    /// Mirrors `RecordDetailDock::IsVisibleForRefresh`: combines
    /// `isHidden()` with the tracked `visibilityChanged` state so
    /// a tabified-but-buried dock also skips work. Offscreen-QPA
    /// fixtures (where `visibilityChanged` never fires) default to
    /// the `isHidden()` answer.
    [[nodiscard]] bool IsVisibleForRefresh() const noexcept;

signals:
    /// User asked to navigate to the anchored row whose source-model
    /// row index is @p sourceRow. Resolved at emit time by walking
    /// the `LogModel` for a matching `AnchorKey`. Argument is -1
    /// when the lookup fails (anchor key has no live row, e.g. the
    /// session was reopened with a different source file).
    void jumpToAnchorRequested(int sourceRow);

#ifdef LOGAPP_BUILD_TESTING
public:
    [[nodiscard]] QListWidget *ListForTest() const noexcept
    {
        return mList;
    }

    [[nodiscard]] QPushButton *ClearAllButtonForTest() const noexcept
    {
        return mClearAllButton;
    }

    /// Unconditional refresh: bypasses `IsVisibleForRefresh()` so
    /// offscreen QPA tests can inspect dock content without first
    /// driving a real `visibilityChanged(true)`. Production callers
    /// must always go through `Refresh()` which respects the gate.
    void RefreshForTest()
    {
        RefreshAlways();
    }
#endif

private:
    /// Resolve the `AnchorManager::Key` carried by @p item back to a
    /// `LogModel` source-row index. -1 when no row in the live model
    /// matches the key (e.g. the anchor outlived its source line).
    [[nodiscard]] int SourceRowForItem(const QListWidgetItem *item) const;

    /// Unconditional refresh-from-anchors-and-theme. Called by
    /// `Refresh()` after the visibility gate passes, by the
    /// `visibilityChanged(true)` handler (the moment the gate
    /// opens), and by `RefreshForTest()` under offscreen QPA.
    void RefreshAlways();

    /// Build the row label "lineId - filename" (with the colour
    /// swatch attached as the icon). Skips the filename portion
    /// when the canonical locator is empty.
    void OnItemActivated(QListWidgetItem *item);
    void OnContextMenuRequested(const QPoint &pos);
    void OnClearAllClicked();

    QPointer<AnchorManager> mAnchors;
    QPointer<LogModel> mModel;
    QPointer<ThemeControl> mTheme;

    QListWidget *mList = nullptr;
    QPushButton *mClearAllButton = nullptr;

    /// Mirrors `RecordDetailDock::mPerceivedVisible`; flips false
    /// when the user buries the dock tab so signal-driven refreshes
    /// elide until the dock is visible again. Default is `false`
    /// because `MainWindow` always adds the dock hidden -- the first
    /// real `visibilityChanged(true)` flips this back on. A `true`
    /// default would let signal-driven refreshes run between
    /// construction and the explicit `hide()` call.
    bool mPerceivedVisible = false;
};
