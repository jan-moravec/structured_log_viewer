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

    /// The active bucket-size rung changed. Argument is the new rung.
    void bucketSizeChanged(int rung);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;

private:
    void ZoomIn();
    void ZoomOut();
    void CancelDrag();

    /// Convert widget X pixel to a bucket index, clamped to the
    /// visible range. Returns `nullopt` when the index is empty or
    /// the X is outside the plot rect.
    [[nodiscard]] std::optional<std::size_t> BucketAtX(int x) const;

    /// Rect in widget coords where bars are painted (excludes the
    /// subtitle line and the small vertical padding).
    [[nodiscard]] QRect PlotRect() const;

    /// Header line above the bars: bucket size + covered range +
    /// total row count.
    [[nodiscard]] QString FormatSubtitle() const;

    /// Refresh a hover tooltip anchored to the current mouse
    /// position. Called from `mouseMoveEvent` and `paintEvent` so
    /// the tooltip stays accurate across data updates.
    void UpdateHoverTooltip(int mouseX);

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
    /// Latest bucket index the mouse was over. `-1` when outside.
    int mLastHoverBucket = -1;

    static constexpr int DRAG_THRESHOLD_PIXELS = 3;
};
