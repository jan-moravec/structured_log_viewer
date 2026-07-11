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

    /// Switch which scrollbar edge is treated as the tail. Always
    /// re-evaluates `mAtTailEdge` against the current scrollbar
    /// position, even when `edge == mTailEdge`: callers rely on the
    /// re-seed to recover from preceding programmatic scrollbar
    /// mutations that the `valueChanged` state machine ignored.
    void SetTailEdge(TailEdge edge);

    [[nodiscard]] TailEdge GetTailEdge() const noexcept;

    /// Non-owning. Required for the anchor slots and helpers to do
    /// anything; without it they're inert (keeps legacy two-arg test
    /// fixtures working).
    void SetAnchorManager(AnchorManager *anchors) noexcept;

    /// `(locator, lineId)` for every selected row. Walks the proxy
    /// chain down to `LogModel`; empty when no `LogModel` is reachable.
    [[nodiscard]] std::vector<AnchorManager::Key> AnchorKeysForSelection() const;

    /// Suppress pending-new-rows counting. `MainWindow` mirrors
    /// `actionFollowTail` into this flag so the pill never flashes
    /// during the at-tail-with-Follow-engaged steady state, where
    /// Qt's signal ordering can briefly drop `mAtTailEdge` between
    /// the row insert and the follow-up scroll-back. Engaging
    /// suppression also clears any pending tally.
    void SetPendingNewRowsSuppressed(bool suppressed);

    /// Zero the running counter and fade the pill out. Called from
    /// the pill-click handler so the announcement clears even when
    /// the resulting scroll lands short of the visual tail (custom
    /// sort placing source-newest in the middle of the proxy, or
    /// the filtered fallback snapping to the proxy tail without
    /// crossing `maximum`).
    void AcknowledgePendingNewRows();

    /// Reserve the right viewport margin for an overview rail
    /// widget. Passing nullptr detaches the current rail and
    /// reclaims the strip. When @p rail is non-null it is
    /// reparented to `this`, positioned inside the reserved
    /// margin, and its geometry is tracked via `resizeEvent` /
    /// `changeEvent(StyleChange, ScreenChangeInternal)`. The
    /// previously-attached rail (if any) is hidden and detached
    /// but not deleted — ownership is the caller's.
    void AttachOverviewRail(QWidget *rail);

    /// Currently-attached overview rail (nullptr when detached).
    /// Non-owning.
    [[nodiscard]] QWidget *OverviewRail() const noexcept
    {
        return mOverviewRail.data();
    }

    /// Current reserved right-margin width in device-independent
    /// px (0 when no rail is attached). Exposed for tests.
    [[nodiscard]] int ReservedRightMargin() const noexcept
    {
        return mReservedRightMargin;
    }

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
    /// to `MainWindow`, which owns the proxy-aware scroll and the
    /// Follow re-engage; the view stays ignorant of both.
    void jumpToTailRequested();

protected:
    /// Draws the empty-state shortcuts card when the model has no rows.
    void paintEvent(QPaintEvent *event) override;

    /// Mark the next `valueChanged` as user-initiated.
    void wheelEvent(QWheelEvent *event) override;

    /// Repositions the overview rail (if attached) and forwards
    /// to the base class for the standard scroll-area layout.
    void resizeEvent(QResizeEvent *event) override;

    /// Refreshes the reserved right margin on style / screen /
    /// font changes so a DPI-fluent rail resizes with the
    /// platform's scrollbar extent.
    void changeEvent(QEvent *event) override;

    /// Watches the viewport for resize events so the floating pill
    /// stays glued to the tail-side edge without subclassing the
    /// viewport.
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    /// Refresh the reserved right margin from the currently
    /// attached rail's `sizeHint().width()` and reposition it to
    /// span the viewport height. No-op when no rail is attached.
    void UpdateOverviewRailGeometry();
    void OnVerticalScrollValueChanged(int value);

    /// Refresh `mAtTailEdge` when the scrollbar range changes
    /// (typically a row insert growing `maximum` without moving
    /// `value`). Without this, a user at the previous tail would
    /// silently drift below it and the pill counter would never
    /// arm. Flag-only: a range change is not a user scroll, so the
    /// `userScrolled*` signals must stay silent.
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

    /// Re-place `mTailPill` against the tail-side viewport edge,
    /// centred horizontally. No-op when the pill is absent.
    void PositionTailPill();

    /// Zero the pending-new-rows counter and fade the pill out.
    /// Called on tail-edge landings, `setModel`, `modelReset`, and
    /// suppression engagement.
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

    /// Floating "N new lines" pill parented to the viewport.
    /// `QPointer` so a viewport teardown zeroes the pointer before
    /// our destructor runs.
    QPointer<JumpToTailPill> mTailPill;

    /// Running count of *visible* (post-filter) rows that arrived
    /// while the user was scrolled away from the tail. Reset on
    /// any tail-edge landing, on model swap, or when suppression
    /// engages.
    int mPendingNewRows = 0;

    /// Mirror of `actionFollowTail`. While set, `OnRowsInserted`
    /// short-circuits even if `mAtTailEdge` momentarily drops to
    /// false between an insert's geometry pass and the follow-up
    /// scroll-back. Kept separate from `mAtTailEdge` because that
    /// flag also feeds the anchor / signal state machines.
    bool mPendingNewRowsSuppressed = false;

    /// Non-owning. `QPointer` so a rail widget destroyed by its
    /// original owner zeroes here before our next geometry pass.
    QPointer<QWidget> mOverviewRail;

    /// Current reserved right-margin width in device-independent
    /// px. Zero when no rail is attached. Cached so
    /// `resizeEvent` can reuse the last known width without
    /// re-querying the rail's `sizeHint`.
    int mReservedRightMargin = 0;

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
    [[nodiscard]] bool PendingNewRowsSuppressedForTest() const noexcept
    {
        return mPendingNewRowsSuppressed;
    }
    /// Direct seam over `mAtTailEdge` so the `rangeChanged`
    /// regression test can observe the transition without
    /// round-tripping through `OnRowsInserted` (which the view
    /// machinery can re-reset before the test reads it).
    [[nodiscard]] bool AtTailEdgeForTest() const noexcept
    {
        return mAtTailEdge;
    }
#endif
};
