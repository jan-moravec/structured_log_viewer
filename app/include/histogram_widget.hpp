#pragma once

#include "histogram_model.hpp"

#include <loglib/histogram_bucket_index.hpp>
#include <loglib/log_value.hpp>

#include <QPointer>
#include <QString>
#include <QWidget>

#include <cstddef>
#include <optional>

class HistogramModel;
class ThemeControl;
class QContextMenuEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QWheelEvent;

/// Bar-chart strip that renders `HistogramModel::Index()` and drives
/// two interactions:
///   - Single click on a bucket -> `bucketClicked(bucketIndex)`.
///   - Click-drag horizontally -> `timeRangeSelected(fromUs, toUs)`
///     on release (epoch microseconds, inclusive).
///
/// The rendering is a paint-by-value `QWidget` (no chart framework)
/// so live-tail updates cost a single `update()` per coalesced batch,
/// and the same technique carries directly to the future overview
/// rail (ROADMAP item 13).
///
/// The widget owns no data — it borrows the `HistogramModel` (which
/// itself borrows `LogModel`) and `ThemeControl`. Both must outlive
/// the widget.
class HistogramWidget : public QWidget
{
    Q_OBJECT

public:
    HistogramWidget(HistogramModel *model, ThemeControl *theme, QWidget *parent = nullptr);

    /// Current details-line text (the strip below the bars).
    /// Reflects whichever branch of `FormatDetailsLine` is active:
    /// the hovered-bucket format when the pointer is over a
    /// populated column, or the plot summary otherwise. Exposed
    /// for `apptest_histogram` so tests can assert on the readout
    /// without racing `QToolTip` visibility under the offscreen
    /// QPA plugin (which is why the old `QToolTip`-based
    /// affordance was replaced by this strip in the first place).
    [[nodiscard]] QString DetailsTextForTest() const;

    /// Cached visual-column index of the bar the pointer last
    /// hovered, or `-1` when the cache is invalidated (leave, data
    /// change, theme change, drag). Same "for tests only" caveat
    /// as `DetailsTextForTest`.
    [[nodiscard]] int LastHoverBucketForTest() const noexcept
    {
        return mLastHoverBucket;
    }

    /// Merged anchor slot mask for visual column @p col — the OR of
    /// every raw bucket the paint routine folds into that column via
    /// the same `ComputeVisualLayout` stride. Empty bitset when the
    /// model isn't wired, the anchor manager is `nullptr`, or @p col
    /// is out of range. Exposed so `apptest_histogram` can assert
    /// that stride > 1 preserves anchor visibility without scraping
    /// pixels.
    [[nodiscard]] HistogramModel::AnchorSlotMask VisualColumnAnchorMaskForTest(std::size_t col) const;

    /// Height of the anchor tick strip currently reserved above the
    /// plot rect (`0` when no bucket carries an anchor). Exposed so
    /// tests can assert the strip's hidden-when-empty behaviour by
    /// numeric height rather than pixel inspection.
    [[nodiscard]] int AnchorTickStripHeightForTest() const;

    /// Read-only accessors mirroring the private geometry helpers.
    /// Exposed so `apptest_histogram` can assert PlotRect stability
    /// across anchor toggles and derive tick-strip click points
    /// without re-implementing the layout logic.
    [[nodiscard]] QRect PlotRectForTest() const
    {
        return PlotRect();
    }

    [[nodiscard]] QRect AnchorTickRectForTest() const
    {
        return AnchorTickRect();
    }

signals:
    /// User clicked (no drag) on a bucket. `bucketIndex` is a valid
    /// index into `HistogramModel::Index().Buckets()` at the moment
    /// the click landed.
    void bucketClicked(std::size_t bucketIndex);

    /// User clicked (no drag) directly on the anchor tick strip
    /// overlaying a column that carries at least one anchor.
    /// `sourceRow` is the earliest anchored source-model row in
    /// that visual column, resolved via
    /// `HistogramModel::FirstAnchoredRowInBucketRange`. Consumers
    /// route this to `MainWindow::SelectSourceRow` so the click
    /// lands on the anchor itself, not just the bucket's first
    /// row. Emitted *instead* of `bucketClicked` for tick-zone
    /// clicks; the bar area continues to route through
    /// `bucketClicked`.
    void anchorClicked(int sourceRow);

    /// User dragged a range and released. Bounds are epoch
    /// microseconds, inclusive. `from <= to` invariantly.
    void timeRangeSelected(qint64 fromEpochMicros, qint64 toEpochMicros);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void ZoomIn();
    void ZoomOut();
    void CancelDrag();

    /// Visual-column index the pixel @p x lands on, clamped to the
    /// visible range. Returns `nullopt` when the index is empty or
    /// the plot rect is degenerate. Kept as the single source of
    /// truth so click / tooltip / drag all agree on the same
    /// stride-aware layout the paint routine renders.
    [[nodiscard]] std::optional<std::size_t> VisualColumnAtX(int x) const;

    /// First raw bucket at or after `columnStart` that has at least
    /// one row. Falls back to `columnStart` when the entire visual
    /// column is empty (paint would skip it anyway; the caller then
    /// surfaces a "no visible row" hint instead of jumping).
    [[nodiscard]] std::size_t FirstNonEmptyBucketInColumn(std::size_t columnIndex) const;

    /// Half-open raw-bucket range `[begin, end)` merged into visual
    /// column @p columnIndex. Empty range when the model is unset or
    /// @p columnIndex is out of range.
    [[nodiscard]] std::pair<std::size_t, std::size_t> BucketRangeForVisualColumn(std::size_t columnIndex) const;

    /// Rect in widget coords where bars are painted (excludes the
    /// details strip along the bottom, the small padding around
    /// the plot area, and the anchor tick strip along the top when
    /// any bucket carries an anchor).
    [[nodiscard]] QRect PlotRect() const;

    /// Rect in widget coords occupied by the anchor tick strip.
    /// The strip is painted as an *overlay* at the top of
    /// `PlotRect` (not reserved chrome above it), so this rect
    /// intersects `PlotRect` when it's non-empty. Empty when no
    /// bucket currently carries an anchor -- a no-anchor session
    /// then paints pixel-identical to the pre-anchor layout.
    /// Kept as a helper so paint, hit-testing, and geometry
    /// inspectors all agree on the strip's extent.
    [[nodiscard]] QRect AnchorTickRect() const;

    /// Paint the anchor tick strip into @p painter over the top of
    /// the plot area. Iterates visual columns with the same stride
    /// the bar pass uses so ticks line up with the columns
    /// underneath. No-op when the model isn't wired, no anchor
    /// exists, or the theme is unset.
    void PaintAnchorTickStrip(QPainter &painter);

    /// Rect in widget coords reserved for the always-visible
    /// details line below the bars. Empty when the widget is too
    /// short to fit it. The strip carries the plot summary in the
    /// idle state and the hovered bucket's details on hover; it
    /// replaces the OS `QToolTip` popup we used earlier.
    [[nodiscard]] QRect DetailsRect() const;

    /// Text painted into `DetailsRect`. If a valid visual column
    /// is hovered, returns the hover format
    /// (`<range> . total: N . <level>: <count> ...`);
    /// otherwise returns the plot summary
    /// (`bucket: <size> . rows: N <range>`).
    [[nodiscard]] QString FormatDetailsLine() const;

    /// Refresh the hover state from the pointer's widget-local
    /// position. Updates `mLastHoverBucket` and requests a partial
    /// repaint of `DetailsRect` when the visual column under the
    /// pointer changes. Called from `mouseMoveEvent` on every
    /// motion outside a drag.
    void UpdateHoverState(const QPoint &pos);

    QPointer<HistogramModel> mModel;
    QPointer<ThemeControl> mTheme;

    /// True while the mouse is pressed and moved past the drag
    /// threshold. Between press and release-without-drag, the
    /// widget reports a click.
    bool mDragging = false;
    /// Anchor pixel of the current drag.
    int mDragStartX = -1;
    /// Latest pixel of the current drag (updated in mouseMove).
    int mDragCurrentX = -1;
    /// Latest visual-column index the mouse was over. `-1` when
    /// outside the plot rect or invalidated by a data / theme
    /// change. Kept in visual-column coords (not raw bucket
    /// indices) so `FormatDetailsLine` reflects exactly the
    /// column the paint routine draws under the pointer.
    int mLastHoverBucket = -1;

    static constexpr int DRAG_THRESHOLD_PIXELS = 3;
};
