#pragma once

#include <QList>
#include <QMetaObject>
#include <QPersistentModelIndex>
#include <QTableView>

class LogTableView : public QTableView
{
    Q_OBJECT

public:
    /// Which edge of the scrollbar represents the "tail" (newest line)
    /// in the current view. `Bottom` is the default append-at-bottom
    /// orientation; `Top` mirrors the layout when the user enabled
    /// **Show newest lines first** in Preferences and a
    /// `StreamOrderProxyModel` reversed the row order.
    enum class TailEdge
    {
        Bottom,
        Top,
    };

    explicit LogTableView(QWidget *parent = nullptr);

    void keyPressEvent(QKeyEvent *event) override;

    /// `QTableView::setModel` is overridden so the view can wire its
    /// own structural-change hooks (rows / layout about-to-be-changed
    /// vs. changed) on whichever model is attached. Used by the
    /// reading-position preservation path described on
    /// `userScrolledAwayFromTail` below.
    void setModel(QAbstractItemModel *model) override;

    /// Switches which edge of the vertical scrollbar `userScrolledTo*`
    /// emissions track. Idempotent: re-evaluates `mAtTailEdge` against
    /// the current scroll position so the next genuine user transition
    /// still edge-triggers correctly.
    void SetTailEdge(TailEdge edge);

    /// Currently configured tail edge.
    [[nodiscard]] TailEdge GetTailEdge() const noexcept;

public slots:
    void CopySelectedRowsToClipboard();

signals:
    /// Emitted when the user manually scrolls **away** from the
    /// configured tail edge (PRD 4.3.3 — VS Code terminal pattern).
    /// Wired to `MainWindow` to auto-disengage the **Follow newest**
    /// toggle. The "tail edge" is the bottom of the view in the
    /// default append-at-bottom orientation and the top of the view
    /// when **Show newest lines first** is enabled.
    ///
    /// Only emitted when the most recent value change was driven by a
    /// user input path (wheel / keyboard / scrollbar action / drag).
    /// Programmatic or layout-driven `valueChanged` events
    /// (`scrollTo` after a new batch, value clamping triggered by
    /// `endInsertRows`, hover-induced internal repaints, our own
    /// scroll-anchor preservation `setValue` below) update the
    /// tracking silently so they can never disengage Follow newest on
    /// their own.
    void userScrolledAwayFromTail();

    /// Emitted when the user manually scrolls back to the configured
    /// tail edge. Wired to `MainWindow` to auto-re-engage the
    /// **Follow newest** toggle (PRD 4.3.3). Same user-input gate as
    /// `userScrolledAwayFromTail`.
    void userScrolledToTail();

protected:
    /// Marks the next `valueChanged` as user-initiated when the wheel
    /// event reaches the viewport.
    void wheelEvent(QWheelEvent *event) override;

private:
    void OnVerticalScrollValueChanged(int value);

    /// Hooks invoked from the attached model's `rowsAboutToBeInserted`
    /// / `rowsInserted` / `layoutAboutToBeChanged` / `layoutChanged`
    /// signals. The "about" half snapshots the topmost visible row as
    /// an anchor (only when the user has scrolled away from the tail
    /// edge); the post-change half restores the anchor's pixel offset
    /// by adjusting the vertical scrollbar. Together they preserve
    /// the user's reading position when new lines arrive at the
    /// visual top in **Show newest lines first** mode (chat-app
    /// pattern).
    void OnRowsAboutToBeInserted(const QModelIndex &parent, int first, int last);
    void OnRowsInserted(const QModelIndex &parent, int first, int last);
    void OnLayoutAboutToBeChanged();
    void OnLayoutChanged();

    /// Computes `(value at the configured tail edge?)` for the current
    /// scrollbar state. Centralised so `SetTailEdge` and
    /// `OnVerticalScrollValueChanged` stay in lock-step.
    [[nodiscard]] bool ComputeAtTailEdge(int value) const;

    /// Captures the topmost visible row as a scroll anchor, but only
    /// when the user is reading older content (`!mAtTailEdge`) in a
    /// reversed orientation (`mTailEdge == TailEdge::Top`). No-op
    /// otherwise; the caller relies on `mAnchorIsSaved` to know
    /// whether a matching restore should run.
    void SaveAnchorIfShouldPreserve();

    /// Restores a previously-saved anchor by adjusting the vertical
    /// scrollbar so the anchor row sits at the same pixel offset as
    /// when it was saved. Programmatic — does not flip the user-input
    /// flag, so it cannot disengage Follow newest on its own.
    void RestoreAnchorIfSaved();

    TailEdge mTailEdge = TailEdge::Bottom;

    /// True while the most recent `valueChanged` was at the configured
    /// tail edge of the scrollbar; lets the slot emit edge-triggered
    /// signals only.
    bool mAtTailEdge = true;

    /// Set by user-input paths (`wheelEvent` / `keyPressEvent`
    /// overrides on the viewport, `actionTriggered` signal on the
    /// vertical scrollbar) and consumed by
    /// `OnVerticalScrollValueChanged`. The `valueChanged` handler
    /// treats programmatic value changes (those observed while this
    /// flag is `false`) as silent so they cannot flip Follow newest
    /// off.
    bool mNextValueChangeIsUser = false;

    /// Persistent index of the topmost visible row at the moment a
    /// pending structural change was about to land. Qt auto-updates
    /// the index across the layout / insertion so we can read its
    /// new pixel position from `RestoreAnchorIfSaved`.
    QPersistentModelIndex mPreservedAnchor;

    /// Pixel offset of `mPreservedAnchor`'s top from the viewport's
    /// top at save time. Usually `<= 0` (the anchor row's top is at
    /// or above the viewport boundary).
    int mPreservedAnchorOffsetPx = 0;

    /// Set when `SaveAnchorIfShouldPreserve` actually captured an
    /// anchor — the matching `RestoreAnchorIfSaved` runs only when
    /// this is true. Avoids double-saves when both `rowsAboutTo…` and
    /// `layoutAboutTo…` fire for the same structural change.
    bool mAnchorIsSaved = false;

    /// Connections we hold against the currently-attached model.
    /// Tracked so `setModel` can drop them cleanly before re-wiring
    /// against a new model.
    QList<QMetaObject::Connection> mModelConnections;
};
