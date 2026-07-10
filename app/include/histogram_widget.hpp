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
///   - Single click on a bar -> `bucketClicked(bucketIndex)`.
///   - Horizontal click-drag -> `timeRangeSelected(fromUs, toUs)` on
///     release (epoch microseconds, inclusive).
///
/// Uses a plain `QWidget::paintEvent` (no chart framework) so live-tail
/// updates cost one `update()` per coalesced batch. Owns no data:
/// borrows `HistogramModel` and `ThemeControl`; both must outlive it.
class HistogramWidget : public QWidget
{
    Q_OBJECT

public:
    HistogramWidget(HistogramModel *model, ThemeControl *theme, QWidget *parent = nullptr);

    /// Current details-strip text (hover format when a column is
    /// hovered, plot summary otherwise). Test-only so tests don't
    /// have to scrape pixels.
    [[nodiscard]] QString DetailsTextForTest() const;

    /// Cached visual-column index of the last hovered bar, or `-1`
    /// when invalidated. Test-only.
    [[nodiscard]] int LastHoverBucketForTest() const noexcept
    {
        return mLastHoverBucket;
    }

    /// Merged anchor slot mask for visual column @p col (folded
    /// through the same stride the paint routine uses). Empty bitset
    /// when the model isn't wired or @p col is out of range. Test-only.
    [[nodiscard]] HistogramModel::AnchorSlotMask VisualColumnAnchorMaskForTest(std::size_t col) const;

    /// Height of the anchor tick strip (0 when no anchor exists).
    /// Test-only.
    [[nodiscard]] int AnchorTickStripHeightForTest() const;

    /// Read-only accessors mirroring the private geometry helpers.
    /// Test-only so tests can derive click points without duplicating
    /// the layout math.
    [[nodiscard]] QRect PlotRectForTest() const
    {
        return PlotRect();
    }

    [[nodiscard]] QRect AnchorTickRectForTest() const
    {
        return AnchorTickRect();
    }

signals:
    /// Bar clicked (no drag). `bucketIndex` is valid in
    /// `HistogramModel::Index().Buckets()` at the moment of the click.
    void bucketClicked(std::size_t bucketIndex);

    /// Tick-strip clicked on a column that carries an anchor.
    /// `sourceRow` is the earliest anchored source-model row in that
    /// column, so consumers can scroll directly to the anchor rather
    /// than the bucket's first row. Emitted instead of `bucketClicked`
    /// for tick-zone clicks.
    void anchorClicked(int sourceRow);

    /// User dragged a range and released. Epoch microseconds,
    /// inclusive, invariantly `from <= to`.
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

    /// Visual-column index for pixel @p x, clamped to the visible
    /// range. `nullopt` when the index is empty or the plot rect is
    /// degenerate. Single source of truth so click / hover / drag
    /// agree with the paint layout.
    [[nodiscard]] std::optional<std::size_t> VisualColumnAtX(int x) const;

    /// First non-empty raw bucket in the visual column @p columnIndex.
    /// Falls back to the column's first bucket when every bucket is
    /// empty (caller then surfaces a "no visible row" hint).
    [[nodiscard]] std::size_t FirstNonEmptyBucketInColumn(std::size_t columnIndex) const;

    /// Half-open raw-bucket range `[begin, end)` merged into visual
    /// column @p columnIndex. Empty range when out of bounds.
    [[nodiscard]] std::pair<std::size_t, std::size_t> BucketRangeForVisualColumn(std::size_t columnIndex) const;

    /// Widget-coord rect where bars are painted. Excludes the outer
    /// padding and the details strip along the bottom. The anchor
    /// tick strip is an overlay *inside* this rect, not reserved above.
    [[nodiscard]] QRect PlotRect() const;

    /// Widget-coord rect for the anchor tick strip. Overlays the top
    /// of `PlotRect`; empty when no bucket carries an anchor (so an
    /// anchor-free session paints pixel-identical to before).
    [[nodiscard]] QRect AnchorTickRect() const;

    /// Paint the anchor tick strip. Uses the same stride as the bar
    /// pass so ticks line up with the columns underneath. No-op when
    /// the model isn't wired, no anchor exists, or the theme is unset.
    void PaintAnchorTickStrip(QPainter &painter);

    /// Widget-coord rect for the always-visible details strip below
    /// the bars. Empty when the widget is too short to fit it.
    [[nodiscard]] QRect DetailsRect() const;

    /// Text painted into `DetailsRect`: hover format when a valid
    /// column is hovered (`<range> . total: N . <level>: <count>...`),
    /// otherwise the plot summary (`bucket: <size> . rows: N <range>`).
    [[nodiscard]] QString FormatDetailsLine() const;

    /// Refresh hover state from pointer position @p pos. Updates
    /// `mLastHoverBucket` and requests a partial repaint of
    /// `DetailsRect` only when the visual column changes.
    void UpdateHoverState(const QPoint &pos);

    QPointer<HistogramModel> mModel;
    QPointer<ThemeControl> mTheme;

    /// True once the mouse has moved past the drag threshold. Between
    /// press and release-without-drag, the widget reports a click.
    bool mDragging = false;
    /// Anchor pixel of the current drag.
    int mDragStartX = -1;
    /// Latest pixel of the current drag (updated in `mouseMoveEvent`).
    int mDragCurrentX = -1;
    /// Last visual-column index the pointer was over. `-1` when
    /// outside the plot rect or invalidated by a data / theme change.
    /// Stored as a visual-column index (not raw bucket) so the details
    /// text matches exactly what paint drew.
    int mLastHoverBucket = -1;

    static constexpr int DRAG_THRESHOLD_PIXELS = 3;
};
