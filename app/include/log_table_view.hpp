#pragma once

#include "anchor_manager.hpp"

#include <QItemSelectionModel>
#include <QList>
#include <QMetaObject>
#include <QPersistentModelIndex>
#include <QTableView>

#include <cstdint>
#include <vector>

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

    /// Paint hook overlays the auto-discovered shortcuts card on top
    /// of the empty grid when no rows are present. Falls through to
    /// the base implementation otherwise.
    void paintEvent(QPaintEvent *event) override;

    /// Override so the view can wire its own structural-change hooks
    /// onto whichever model is attached (used by the reading-position
    /// preservation path -- see `userScrolledAwayFromTail`).
    void setModel(QAbstractItemModel *model) override;

    /// Switch which scrollbar edge is treated as the tail. Idempotent;
    /// re-evaluates `mAtTailEdge` against the current position.
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

protected:
    /// Mark the next `valueChanged` as user-initiated.
    void wheelEvent(QWheelEvent *event) override;

private:
    void OnVerticalScrollValueChanged(int value);

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
};
