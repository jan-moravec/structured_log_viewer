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
/// the source path's filename, and -- when readily available -- a
/// short summary of the first non-empty column value. Double-click
/// emits `jumpToAnchorRequested(sourceRow)`; right-click offers
/// "Remove anchor". A header button wipes every anchor via the
/// owning `AnchorManager`.
///
/// The dock listens to `AnchorManager::anchorChanged` and
/// `anchorsReset` so the list stays in lockstep with the model
/// without polling. Refresh work is gated on `IsVisibleForRefresh()`
/// so a hidden / buried-tab dock does no list rebuilding.
///
/// Ownership: the `AnchorManager`, `LogModel`, and `ThemeControl`
/// are borrowed; all three must outlive the dock. The dock parents
/// itself to its owning `MainWindow` and is destroyed with it.
class AnchorsDock : public QDockWidget
{
    Q_OBJECT

public:
    AnchorsDock(AnchorManager *anchors, LogModel *model, ThemeControl *theme, QWidget *parent = nullptr);

    /// Force a refresh from `AnchorManager::Entries()`. Production
    /// code never calls this directly; the dock auto-refreshes on
    /// anchor signals. Tests invoke it to short-circuit the
    /// visibility gate (offscreen QPA fixtures keep the dock
    /// hidden, so signal-driven refreshes elide on `isHidden`).
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
#endif

private:
    /// Resolve the `AnchorManager::Key` carried by @p item back to a
    /// `LogModel` source-row index. -1 when no row in the live model
    /// matches the key (e.g. the anchor outlived its source line).
    [[nodiscard]] int SourceRowForItem(const QListWidgetItem *item) const;

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
    /// elide until the dock is visible again.
    bool mPerceivedVisible = true;
};
