#pragma once

#include "anchor_manager.hpp"
#include "jump_to_tail_pill.hpp"

#include <QHeaderView>
#include <QItemSelectionModel>
#include <QList>
#include <QMetaObject>
#include <QPersistentModelIndex>
#include <QPointer>
#include <QStyleOptionHeader>
#include <QTableView>

#include <cstdint>
#include <vector>

/// Horizontal header that centres the icon in icon-only sections
/// (e.g. the level column in icon mode), so the header glyph lines
/// up with the centred pills painted below. Qt's default
/// `iconAlignment` is left-aligned, which would leave the icon
/// hugging the section's left edge.
class LogHeaderView : public QHeaderView
{
public:
    using QHeaderView::QHeaderView;

    /// If @p option has an icon and no text, flip `iconAlignment`
    /// to `Qt::AlignCenter`. Exposed as a static pure transform so
    /// tests can exercise the rule without a model.
    static void CenterIconAlignmentForIconOnlySection(QStyleOptionHeader *option);

protected:
    void initStyleOptionForIndex(QStyleOptionHeader *option, int logicalIndex) const override;
};

class LogTableView : public QTableView
{
    Q_OBJECT

public:
    /// Which scrollbar edge represents the "tail" (newest line). `Bottom`
    /// is the default append-at-bottom orientation; `Top` mirrors the
    /// layout when **Show newest lines first** is enabled.
    enum class TailEdge
    {
        Bottom,
        Top,
    };

    explicit LogTableView(QWidget *parent = nullptr);

    void keyPressEvent(QKeyEvent *event) override;

    /// Override so the view can wire its own structural-change hooks
    /// onto whichever model is attached (used by the reading-position
    /// preservation path -- see `userScrolledAwayFromTail`).
    void setModel(QAbstractItemModel *model) override;

    /// Switch which scrollbar edge is treated as the tail.
    /// **Always** re-evaluates `mAtTailEdge` against the current
    /// scrollbar position, even when `edge == mTailEdge` -- callers
    /// (and tests) rely on the re-seed side effect to recover from a
    /// preceding programmatic scrollbar mutation that the
    /// `valueChanged`-driven state machine silently absorbed. Do not
    /// short-circuit on `edge == mTailEdge` without also providing a
    /// separate re-seed seam.
    void SetTailEdge(TailEdge edge);

    [[nodiscard]] TailEdge GetTailEdge() const noexcept;

    /// Non-owning. Required for the anchor slots and helpers to do
    /// anything; without it they're inert (keeps legacy two-arg test
    /// fixtures working).
    void SetAnchorManager(AnchorManager *anchors) noexcept;

    /// `(locator, lineId)` for every selected row. Walks the proxy
    /// chain down to `LogModel`; empty when no `LogModel` is reachable.
    [[nodiscard]] std::vector<AnchorManager::Key> AnchorKeysForSelection() const;

public slots:
    void CopySelectedRowsToClipboard();

    /// Anchor every selected row at slot @p colorIndex (adds or
    /// recolours). No-op when nothing is wired or selected.
    void AnchorSelection(int colorIndex);

    /// Remove the anchor from every selected row. Silently skips
    /// rows that aren't anchored.
    void ClearAnchorOnSelection();

#ifdef LOGAPP_BUILD_TESTING
public:
    /// Test seam over the protected `selectionCommand`; lets the
    /// row-click-semantics regression test inspect selection flags
    /// without synthesising real mouse events.
    [[nodiscard]] QItemSelectionModel::SelectionFlags SelectionCommandForTest(
        const QModelIndex &index, const QEvent *event = nullptr
    ) const
    {
        return selectionCommand(index, event);
    }
#endif

signals:
    /// User manually scrolled away from the configured tail edge.
    /// Wired to `MainWindow` to auto-disengage Follow newest.
    /// Programmatic value changes (`scrollTo`, `endInsertRows`
    /// clamping, our own anchor restore) are filtered out so they
    /// cannot disengage Follow newest on their own.
    void userScrolledAwayFromTail();

    /// User manually scrolled back to the tail edge. Wired to
    /// `MainWindow` to auto-re-engage Follow newest. Same user-input
    /// filter as `userScrolledAwayFromTail`.
    void userScrolledToTail();

    /// User clicked the floating "jump to newest" pill. Forwarded
    /// to `MainWindow`, which performs the proxy-aware scroll and
    /// (in live-tail sessions) re-engages Follow newest. The view
    /// itself stays ignorant of proxies and the Follow action.
    void jumpToTailRequested();

protected:
    /// Draws the empty-state shortcuts card when the model has no rows.
    void paintEvent(QPaintEvent *event) override;

    /// Mark the next `valueChanged` as user-initiated.
    void wheelEvent(QWheelEvent *event) override;

    /// Filters viewport resize events so the floating pill stays
    /// glued to the tail-side edge of the visible area without the
    /// view having to subclass the viewport. Installed in
    /// `LogTableView::LogTableView`.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void OnVerticalScrollValueChanged(int value);

    /// Re-evaluate `mAtTailEdge` when the scrollbar's range changes
    /// (typically because new rows grew `maximum`). Without this, a
    /// user sitting at the previous tail can drift below it silently:
    /// `valueChanged` never fires (the value didn't move), but
    /// `value < newMax` now -- so `mAtTailEdge` would stay stuck at
    /// `true` and `OnRowsInserted` would keep skipping the count.
    /// Transitions are flag-only here: a range change is not a user
    /// scroll, so the `userScrolled*` signals stay silent (Follow
    /// newest must not be toggled by a layout event).
    void OnVerticalScrollRangeChanged(int min, int max);

    /// Reading-position preservation hooks (chat-app pattern). Around
    /// each structural change we snapshot the topmost visible row and
    /// restore its pixel offset afterwards, so the user's view stays
    /// stable while new rows arrive at the top in newest-first mode.
    /// No-ops when the user is at the tail edge or when the
    /// orientation is the default `TailEdge::Bottom`.
    void OnRowsAboutToBeInserted(const QModelIndex &parent, int first, int last);
    void OnRowsInserted(const QModelIndex &parent, int first, int last);
    void OnLayoutAboutToBeChanged();
    void OnLayoutChanged();

    [[nodiscard]] bool ComputeAtTailEdge(int value) const;

    void SaveAnchorIfShouldPreserve();
    void RestoreAnchorIfSaved();

    /// Re-place `mTailPill` at the tail-side edge of the
    /// viewport, centred horizontally. Called from the viewport
    /// resize hook and from `SetTailEdge` (when the edge flips).
    /// No-op when the pill is absent.
    void PositionTailPill();

    /// Centralised reset: zero the pending-new-rows counter and
    /// fade the pill out. Called when the user / a programmatic
    /// edge change lands the viewport back at the tail, when a
    /// new model is attached, on `modelReset`, and on session
    /// boundaries that drop the table contents.
    void ResetPendingNewRows();

    TailEdge mTailEdge = TailEdge::Bottom;

    /// True while the scrollbar is at the configured tail edge; lets
    /// `OnVerticalScrollValueChanged` emit edge-triggered signals.
    bool mAtTailEdge = true;

    /// Set by user-input paths (wheel, key press, scrollbar action)
    /// and consumed by `OnVerticalScrollValueChanged`. Programmatic
    /// value changes (this flag still false) are silent.
    bool mNextValueChangeIsUser = false;

    /// Topmost visible row captured before a pending structural change.
    /// Qt updates the persistent index across the layout / insertion,
    /// so we can read its new pixel position on the way out.
    QPersistentModelIndex mPreservedAnchor;

    /// Pixel offset of the anchor row's top from the viewport top at
    /// save time. Typically <= 0.
    int mPreservedAnchorOffsetPx = 0;

    /// True iff `SaveAnchorIfShouldPreserve` captured an anchor; the
    /// matching `RestoreAnchorIfSaved` runs only then.
    bool mAnchorIsSaved = false;

    /// Connections to the currently-attached model; dropped on
    /// `setModel` before re-wiring.
    QList<QMetaObject::Connection> mModelConnections;

    /// Non-owning. Wired by `MainWindow`; null on standalone test
    /// fixtures. Anchor slots null-check before dereferencing.
    AnchorManager *mAnchors = nullptr;

    /// Floating "* N new lines" pill parented to the viewport.
    /// `QPointer` so a viewport teardown (Qt destroys the
    /// viewport when the view is destroyed) zeroes the pointer
    /// before our own destructor runs, sparing the slot wiring a
    /// dangling deref. Visibility / count are driven by
    /// `OnRowsInserted`, `OnVerticalScrollValueChanged`, and the
    /// `modelReset` hook.
    QPointer<JumpToTailPill> mTailPill;

    /// Running count of rows appended while the user was scrolled
    /// away from the tail edge. Reset to 0 whenever the viewport
    /// returns to the tail edge or the model is wiped.
    int mPendingNewRows = 0;

#ifdef LOGAPP_BUILD_TESTING
public:
    [[nodiscard]] JumpToTailPill *TailPillForTest() const noexcept
    {
        return mTailPill;
    }
    [[nodiscard]] int PendingNewRowsForTest() const noexcept
    {
        return mPendingNewRows;
    }
    /// `mAtTailEdge` is the at-tail-edge flag the scroll-edge state
    /// machine maintains. The `OnVerticalScrollRangeChanged` listener
    /// refreshes it when row inserts grow the range without moving
    /// the value; this seam lets the corresponding regression test
    /// observe the transition without round-tripping through
    /// `OnRowsInserted` (which Qt's view machinery can re-reset
    /// before the test gets a chance to read it).
    [[nodiscard]] bool AtTailEdgeForTest() const noexcept
    {
        return mAtTailEdge;
    }
#endif
};
