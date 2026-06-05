#pragma once

#include "anchor_manager.hpp"

#include <QDockWidget>
#include <QPointer>

class LogModel;
class ThemeControl;
class QCloseEvent;
class QListWidget;
class QListWidgetItem;
class QPushButton;

/// Dockable list of every anchored row. Each entry shows a colour
/// swatch, the row's `lineId`, and the source filename (when known).
/// Double-click jumps to the row via `jumpToAnchorRequested`;
/// right-click offers Jump / Remove. A header button clears everything.
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

    /// Emitted when the user actually dismisses the dock (X
    /// button, system close). Not emitted on tab inactivation,
    /// so the View-menu toggle keyed off this signal stays
    /// accurate even when the dock is tabified with another.
    void closed();

protected:
    /// Emit `closed` after the base class accepts the close so
    /// `QDockWidget::visibilityChanged(false)` -- which also
    /// fires on tab switches -- doesn't have to disambiguate
    /// "user closed me" from "I'm just buried under a sibling
    /// tab".
    void closeEvent(QCloseEvent *event) override;

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

    /// Unconditional refresh for offscreen-QPA tests that can't drive
    /// real `visibilityChanged` events.
    void RefreshForTest()
    {
        RefreshAlways();
    }
#endif

private:
    /// Resolve @p item's key back to a `LogModel` source-row index, or
    /// -1 if the anchor outlived its row.
    [[nodiscard]] int SourceRowForItem(const QListWidgetItem *item) const;

    /// Rebuild from `AnchorManager::Entries()` unconditionally.
    void RefreshAlways();

    void OnItemActivated(QListWidgetItem *item);
    void OnContextMenuRequested(const QPoint &pos);
    void OnClearAllClicked();

    QPointer<AnchorManager> mAnchors;
    QPointer<LogModel> mModel;
    QPointer<ThemeControl> mTheme;

    QListWidget *mList = nullptr;
    QPushButton *mClearAllButton = nullptr;

    /// Tracks `visibilityChanged` so a buried tabified dock also skips
    /// signal-driven refreshes. Starts false because the dock is added
    /// hidden; flipped by the first `visibilityChanged(true)`.
    bool mPerceivedVisible = false;
};
