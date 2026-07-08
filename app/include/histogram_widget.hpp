#pragma once

#include <loglib/histogram_bucket_index.hpp>
#include <loglib/log_value.hpp>

#include <QPointer>
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

signals:
    /// User clicked (no drag) on a bucket. `bucketIndex` is a valid
    /// index into `HistogramModel::Index().Buckets()` at the moment
    /// the click landed.
    void bucketClicked(std::size_t bucketIndex);

    /// User dragged a range and released. Bounds are epoch
    /// microseconds, inclusive. `from <= to` invariantly.
    void timeRangeSelected(qint64 fromEpochMicros, qint64 toEpochMicros);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
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
    /// subtitle line and the small vertical padding).
    [[nodiscard]] QRect PlotRect() const;

    /// Header line above the bars: bucket size + covered range +
    /// total row count.
    [[nodiscard]] QString FormatSubtitle() const;

    /// Refresh the hover tooltip using the pointer's widget-local
    /// position. The X coordinate drives visual-column resolution;
    /// the Y is only used to anchor the tooltip so the OS placement
    /// heuristics can pick a side that doesn't occlude the bar under
    /// the pointer. Called from `mouseMoveEvent` on every motion.
    void UpdateHoverTooltip(const QPoint &pos);

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
    /// indices) so the tooltip dedup matches what the paint routine
    /// draws under the pointer.
    int mLastHoverBucket = -1;

    static constexpr int DRAG_THRESHOLD_PIXELS = 3;
};
