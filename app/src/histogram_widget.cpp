#include "histogram_widget.hpp"

#include "histogram_model.hpp"
#include "theme_control.hpp"

#include <loglib/histogram_bucket_index.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_processing.hpp>
#include <loglib/log_value.hpp>

#include <date/date.h>
#include <date/tz.h>

#include <QBrush>
#include <QColor>
#include <QContextMenuEvent>
#include <QEvent>
#include <QKeyEvent>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QPalette>
#include <QRect>
#include <QString>
#include <QToolTip>
#include <QWheelEvent>
#include <Qt>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace
{

/// Painted stack order, bottom to top. Level with the higher severity
/// paints on top so error / fatal spikes are visually dominant.
/// `Unknown` sits at the bottom so unstyled rows are visible but
/// don't hide meaningful segments. Size is derived from
/// `CANONICAL_LEVEL_COUNT` so a new canonical level in `loglib` is
/// caught at compile time here (the `static_assert` below also pins
/// the count so a bare `+= 1` in `LogLevel` without an entry here
/// fails the build).
constexpr std::array<loglib::LogLevel, loglib::CANONICAL_LEVEL_COUNT + 1> STACK_ORDER = {
    loglib::LogLevel::Unknown,
    loglib::LogLevel::Trace,
    loglib::LogLevel::Debug,
    loglib::LogLevel::Info,
    loglib::LogLevel::Warn,
    loglib::LogLevel::Error,
    loglib::LogLevel::Fatal,
};
static_assert(
    STACK_ORDER.size() == loglib::CANONICAL_LEVEL_COUNT + 1,
    "STACK_ORDER must cover Unknown + every canonical LogLevel"
);

constexpr int SUBTITLE_HEIGHT = 18;
constexpr int PLOT_TOP_PADDING = 20;
constexpr int PLOT_BOTTOM_PADDING = 4;
constexpr int PLOT_HORIZONTAL_PADDING = 4;

/// Minimum widget height. Enough to fit the subtitle line and a
/// visible-but-cramped bar area on the tightest layouts.
constexpr int MIN_WIDGET_HEIGHT = 96;

/// Alpha channel for the drag-brush overlay (0-255). Kept subtle so
/// the underlying bars stay readable through the highlight tint.
constexpr int DRAG_BRUSH_ALPHA = 80;

/// Fallback colour for a level slot the active theme leaves
/// unstyled. Matches the historical tint the row-styling code falls
/// back to when `ThemeControl::BackgroundFor` returns an invalid
/// brush.
QColor FallbackColorFor(loglib::LogLevel level)
{
    switch (level)
    {
    case loglib::LogLevel::Fatal:
        return {"#c0392b"};
    case loglib::LogLevel::Error:
        return {"#e74c3c"};
    case loglib::LogLevel::Warn:
        return {"#f1c40f"};
    case loglib::LogLevel::Info:
        return {"#3498db"};
    case loglib::LogLevel::Debug:
        return {"#95a5a6"};
    case loglib::LogLevel::Trace:
        return {"#7f8c8d"};
    case loglib::LogLevel::Unknown:
        break;
    }
    return {"#606060"};
}

QColor ColorForLevel(const ThemeControl *theme, loglib::LogLevel level)
{
    if (theme != nullptr)
    {
        const QBrush brush = theme->BackgroundFor(level);
        if (brush.style() != Qt::NoBrush)
        {
            return brush.color();
        }
    }
    return FallbackColorFor(level);
}

/// Format string suitable for `date::format` given the bucket rung.
/// Precision matches the bucket width so a 1 h rung doesn't advertise
/// fake sub-hour precision. Kept aligned with the shape `log_table.cpp`
/// uses for its `%F %T` cells so the table and the strip agree.
[[nodiscard]] const char *DateFormatForZoom(loglib::HistogramBucketSize size) noexcept
{
    switch (size)
    {
    case loglib::HistogramBucketSize::OneSecond:
    case loglib::HistogramBucketSize::TenSeconds:
        return "%F %T";
    case loglib::HistogramBucketSize::OneMinute:
    case loglib::HistogramBucketSize::TenMinutes:
        return "%F %R";
    case loglib::HistogramBucketSize::OneHour:
        return "%F %H:00";
    case loglib::HistogramBucketSize::OneDay:
        return "%F";
    }
    return "%F %T";
}

/// Render @p ts as a local-time string using the same `date::CurrentZone`
/// / `date::format` pipeline the log table uses, so the histogram and
/// the table agree on how a timestamp reads. Second precision is enough
/// for every rung we ship (finest is 1 s), so we round down and skip
/// sub-second formatting entirely.
QString FormatLocalTimestampForZoom(loglib::TimeStamp ts, loglib::HistogramBucketSize size)
{
    const auto seconds = std::chrono::floor<std::chrono::seconds>(ts);
    const date::zoned_time localTime{loglib::CurrentZone(), seconds};
    const std::string formatted = date::format(DateFormatForZoom(size), localTime);
    return QString::fromStdString(formatted);
}

QString BucketSizeLabelQt(loglib::HistogramBucketSize size)
{
    const auto label = loglib::HistogramBucketSizeLabel(size);
    return QString::fromLatin1(label.data(), static_cast<qsizetype>(label.size()));
}

QString CanonicalLevelNameQt(loglib::LogLevel level)
{
    const auto name = loglib::CanonicalLevelName(level);
    return QString::fromLatin1(name.data(), static_cast<qsizetype>(name.size()));
}

/// Bar-width policy: `stride` is how many raw buckets fold into one
/// visual column when the widget is narrower than the bucket count.
/// Kept as a free helper so `paintEvent` and hit-testing agree on the
/// layout exactly.
struct VisualLayout
{
    std::size_t stride = 1;
    std::size_t columnCount = 0;
    double columnWidth = 0.0;
};

[[nodiscard]] VisualLayout ComputeVisualLayout(std::size_t nBuckets, double plotWidth) noexcept
{
    VisualLayout layout;
    if (nBuckets == 0 || plotWidth <= 0.0)
    {
        return layout;
    }
    layout.stride = std::max<std::size_t>(
        1, static_cast<std::size_t>(std::ceil(static_cast<double>(nBuckets) / plotWidth))
    );
    layout.columnCount = (nBuckets + layout.stride - 1) / layout.stride;
    layout.columnWidth = plotWidth / static_cast<double>(layout.columnCount);
    return layout;
}

/// Half-open bucket range `[begin, end)` merged into visual column @p col.
[[nodiscard]] std::pair<std::size_t, std::size_t> BucketRangeForColumn(
    std::size_t col, const VisualLayout &layout, std::size_t nBuckets
) noexcept
{
    const std::size_t begin = col * layout.stride;
    const std::size_t end = std::min(nBuckets, begin + layout.stride);
    return {begin, end};
}

/// Sum the raw buckets in `[begin, end)` into one merged column.
[[nodiscard]] loglib::LevelBucket MergeBuckets(
    std::span<const loglib::LevelBucket> buckets, std::size_t begin, std::size_t end
) noexcept
{
    loglib::LevelBucket merged{};
    for (std::size_t i = begin; i < end; ++i)
    {
        for (std::size_t s = 0; s < merged.counts.size(); ++s)
        {
            merged.counts[s] += buckets[i].counts[s];
        }
    }
    return merged;
}

} // namespace

HistogramWidget::HistogramWidget(HistogramModel *model, ThemeControl *theme, QWidget *parent)
    : QWidget(parent), mModel(model), mTheme(theme)
{
    setObjectName(QStringLiteral("histogramWidget"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumHeight(MIN_WIDGET_HEIGHT);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Base);

    if (mModel != nullptr)
    {
        // Any data mutation invalidates the hover-tooltip cache: the
        // bucket the pointer currently sits over may now report
        // different counts (streaming batch), a different label
        // (rung change), or not exist at all (retention eviction).
        // Without this reset the dedup guard on `mLastHoverBucket`
        // would leave a stale tooltip on screen until the mouse moves.
        connect(mModel, &HistogramModel::bucketsChanged, this, [this]() {
            mLastHoverBucket = -1;
            update();
        });
        connect(mModel, &HistogramModel::timeColumnAvailabilityChanged, this, [this](bool /*hasTime*/) {
            mLastHoverBucket = -1;
            update();
        });
    }
    if (mTheme != nullptr)
    {
        // Same reasoning: a theme flip changes the bar palette; drop
        // the cached hover bucket so the next mouse move rebuilds
        // the tooltip in the new colours.
        connect(mTheme, &ThemeControl::themeChanged, this, [this]() {
            mLastHoverBucket = -1;
            update();
        });
    }
}

QRect HistogramWidget::PlotRect() const
{
    const int x = PLOT_HORIZONTAL_PADDING;
    const int y = PLOT_TOP_PADDING;
    const int w = std::max(0, width() - (2 * PLOT_HORIZONTAL_PADDING));
    const int h = std::max(0, height() - PLOT_TOP_PADDING - PLOT_BOTTOM_PADDING);
    return {x, y, w, h};
}

QString HistogramWidget::FormatSubtitle() const
{
    if (mModel == nullptr)
    {
        return {};
    }
    const auto &idx = mModel->Index();
    const QString bucketLabel = BucketSizeLabelQt(idx.BucketSize());
    QString rangePart;
    if (!idx.Empty())
    {
        const auto first = idx.BucketStart(0);
        const auto last = idx.BucketEnd(idx.Buckets().size() - 1);
        rangePart = QStringLiteral("  %1 \u2013 %2")
                        .arg(FormatLocalTimestampForZoom(first, idx.BucketSize()))
                        .arg(FormatLocalTimestampForZoom(last, idx.BucketSize()));
    }
    const QLocale locale = QLocale::system();
    return QStringLiteral("bucket: %1  \u00b7  rows: %2%3")
        .arg(bucketLabel)
        .arg(locale.toString(static_cast<qulonglong>(idx.TotalRowCount())))
        .arg(rangePart);
}

void HistogramWidget::paintEvent(QPaintEvent *event)
{
    // No partial-repaint bookkeeping: `paintEvent` re-renders the
    // whole strip on every call. The parameter is required by the
    // Qt override signature but intentionally unused.
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const QRect plotRect = PlotRect();

    // Subtitle line at the top. Elide with the current font metrics
    // so a narrow dock (or a wide range that fills the label) truncates
    // with an ellipsis instead of clipping mid-glyph.
    const QRect subtitleRect(PLOT_HORIZONTAL_PADDING, 0, width() - (2 * PLOT_HORIZONTAL_PADDING), SUBTITLE_HEIGHT);
    painter.setPen(palette().color(QPalette::WindowText));
    const QString subtitle = FormatSubtitle();
    const QString elidedSubtitle = fontMetrics().elidedText(subtitle, Qt::ElideRight, subtitleRect.width());
    painter.drawText(subtitleRect, Qt::AlignVCenter | Qt::AlignLeft, elidedSubtitle);

    // Empty-state placeholder: no model, no time column, empty
    // index, or a plot rect too small to render bars into.
    const bool empty = mModel == nullptr || !mModel->HasTimeColumn() || mModel->Index().Empty() ||
                       plotRect.height() <= 0 || plotRect.width() <= 0;
    if (empty)
    {
        painter.setPen(palette().color(QPalette::PlaceholderText));
        QString message;
        if (mModel == nullptr)
        {
            message = tr("Histogram unavailable.");
        }
        else if (!mModel->HasTimeColumn())
        {
            message = tr("This log has no time column \u2014 histogram unavailable.");
        }
        else
        {
            message = tr("No rows to bucket yet.");
        }
        painter.drawText(plotRect, Qt::AlignCenter, message);
        return;
    }

    const auto &idx = mModel->Index();
    const auto buckets = idx.Buckets();
    const std::size_t nBuckets = buckets.size();

    const double plotWidth = plotRect.width();
    const double plotHeight = plotRect.height();

    // Aggregate consecutive buckets into single visual columns when
    // the bar width would drop below 1 px. `stride` is 1 when every
    // bucket gets its own pixel or more.
    const VisualLayout layout = ComputeVisualLayout(nBuckets, plotWidth);
    if (layout.columnCount == 0)
    {
        return;
    }

    // Precompute the merged totals up-front so `maxTotal` reflects
    // the tallest *visible* column, not the tallest raw bucket. When
    // `stride > 1` the two disagree by up to a factor of `stride`,
    // and using raw `maxTotal` would let aggregated columns overshoot
    // the plot rect and paint over the subtitle. The extra pass is
    // O(nBuckets) — a rounding-error cost next to the per-column
    // painter fills below.
    std::vector<loglib::LevelBucket> merged;
    merged.reserve(layout.columnCount);
    uint32_t maxTotal = 0;
    for (std::size_t col = 0; col < layout.columnCount; ++col)
    {
        const auto [begin, end] = BucketRangeForColumn(col, layout, nBuckets);
        merged.push_back(MergeBuckets(buckets, begin, end));
        maxTotal = std::max(maxTotal, merged.back().Total());
    }
    if (maxTotal == 0)
    {
        return;
    }

    for (std::size_t col = 0; col < layout.columnCount; ++col)
    {
        const uint32_t total = merged[col].Total();
        if (total == 0)
        {
            continue;
        }
        const double columnX = plotRect.left() + (static_cast<double>(col) * layout.columnWidth);
        const double columnPixelWidth = std::max(1.0, layout.columnWidth - 0.5);
        const double totalHeight = plotHeight * (static_cast<double>(total) / static_cast<double>(maxTotal));
        double stackTop = plotRect.bottom() + 1.0 - totalHeight;
        for (const loglib::LogLevel level : STACK_ORDER)
        {
            const uint32_t count = merged[col].counts[static_cast<std::size_t>(level)];
            if (count == 0)
            {
                continue;
            }
            const double segmentHeight = plotHeight * (static_cast<double>(count) / static_cast<double>(maxTotal));
            const QRectF segment(columnX, stackTop, columnPixelWidth, segmentHeight);
            painter.fillRect(segment, ColorForLevel(mTheme, level));
            stackTop += segmentHeight;
        }
    }

    // Draw the drag brush overlay on top. Both fill and frame use
    // `QRectF` on the same rectangle so the outline stays symmetric
    // (the previous `.adjusted(0, 0, -1, -1)` on the integer rect
    // shrank the frame by one pixel at the right and bottom edges).
    if (mDragging)
    {
        const qreal x1 = std::min(mDragStartX, mDragCurrentX);
        const qreal x2 = std::max(mDragStartX, mDragCurrentX);
        const QRectF brushRect(x1, plotRect.top(), std::max<qreal>(1.0, x2 - x1), plotRect.height());
        QColor brushColor = palette().color(QPalette::Highlight);
        brushColor.setAlpha(DRAG_BRUSH_ALPHA);
        painter.fillRect(brushRect, brushColor);
        QPen framePen(palette().color(QPalette::Highlight));
        framePen.setCosmetic(true);
        painter.setPen(framePen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(brushRect);
    }
}

std::optional<std::size_t> HistogramWidget::VisualColumnAtX(int x) const
{
    if (mModel == nullptr || mModel->Index().Empty())
    {
        return std::nullopt;
    }
    const QRect plotRect = PlotRect();
    if (plotRect.width() <= 0)
    {
        return std::nullopt;
    }
    const std::size_t nBuckets = mModel->Index().Buckets().size();
    const VisualLayout layout = ComputeVisualLayout(nBuckets, static_cast<double>(plotRect.width()));
    if (layout.columnCount == 0)
    {
        return std::nullopt;
    }
    const int clampedX = std::clamp(x, plotRect.left(), plotRect.right());
    const double norm = static_cast<double>(clampedX - plotRect.left()) / static_cast<double>(plotRect.width());
    auto columnIdx = static_cast<std::size_t>(norm * static_cast<double>(layout.columnCount));
    if (columnIdx >= layout.columnCount)
    {
        columnIdx = layout.columnCount - 1;
    }
    return columnIdx;
}

std::pair<std::size_t, std::size_t> HistogramWidget::BucketRangeForVisualColumn(std::size_t columnIndex) const
{
    if (mModel == nullptr)
    {
        return {0, 0};
    }
    const std::size_t nBuckets = mModel->Index().Buckets().size();
    const QRect plotRect = PlotRect();
    const VisualLayout layout = ComputeVisualLayout(nBuckets, static_cast<double>(std::max(0, plotRect.width())));
    if (columnIndex >= layout.columnCount)
    {
        return {0, 0};
    }
    return BucketRangeForColumn(columnIndex, layout, nBuckets);
}

std::size_t HistogramWidget::FirstNonEmptyBucketInColumn(std::size_t columnIndex) const
{
    const auto [begin, end] = BucketRangeForVisualColumn(columnIndex);
    if (begin >= end)
    {
        return begin;
    }
    const auto buckets = mModel->Index().Buckets();
    for (std::size_t i = begin; i < end; ++i)
    {
        if (buckets[i].Total() > 0)
        {
            return i;
        }
    }
    // Every raw bucket in the visual column is empty. Return the
    // column's first bucket so the click still resolves to a stable
    // index; `HistogramModel::FirstRowInBucket` will report `-1` and
    // the main window will surface the "no visible row" toast.
    return begin;
}

void HistogramWidget::UpdateHoverTooltip(const QPoint &pos)
{
    if (mModel == nullptr)
    {
        QToolTip::hideText();
        return;
    }
    const auto columnOpt = VisualColumnAtX(pos.x());
    if (!columnOpt.has_value())
    {
        QToolTip::hideText();
        mLastHoverBucket = -1;
        return;
    }
    const std::size_t columnIdx = *columnOpt;
    if (std::cmp_equal(columnIdx, mLastHoverBucket))
    {
        return;
    }
    mLastHoverBucket = static_cast<int>(columnIdx);
    const auto &idx = mModel->Index();
    const auto [begin, end] = BucketRangeForVisualColumn(columnIdx);
    if (begin >= end)
    {
        QToolTip::hideText();
        return;
    }
    // Tooltip reflects the *visual* column, so multi-bucket columns
    // (stride > 1) show the merged counts the user actually sees.
    const loglib::LevelBucket merged = MergeBuckets(idx.Buckets(), begin, end);
    const auto start = idx.BucketStart(begin);
    // `end` is exclusive in raw-bucket coords; the visible span
    // ends at the *end* of the last merged bucket, i.e. `end - 1`.
    const auto stop = idx.BucketEnd(end - 1);
    QString body = tr("%1 \u2013 %2\ntotal: %3")
                       .arg(FormatLocalTimestampForZoom(start, idx.BucketSize()))
                       .arg(FormatLocalTimestampForZoom(stop, idx.BucketSize()))
                       .arg(merged.Total());
    for (auto level : STACK_ORDER)
    {
        const uint32_t count = merged.counts[static_cast<std::size_t>(level)];
        if (count == 0)
        {
            continue;
        }
        body.append(QStringLiteral("\n"));
        body.append(CanonicalLevelNameQt(level));
        body.append(QStringLiteral(": "));
        body.append(QString::number(count));
    }
    // Anchor at the pointer so the OS's tooltip placement picks a
    // side that doesn't occlude the bar under the cursor. The old
    // anchor at `y = PLOT_TOP_PADDING` sat over the tallest part of
    // the tallest bar.
    QToolTip::showText(mapToGlobal(pos), body, this);
}

void HistogramWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        QWidget::mousePressEvent(event);
        return;
    }
    // Reject presses on the subtitle strip and the outer padding:
    // the user hasn't pointed at a bar, so treating this as a click
    // (or the anchor of a drag) would install a filter or jump the
    // table from an accidental touch on the header line.
    if (!PlotRect().contains(event->pos()))
    {
        QWidget::mousePressEvent(event);
        return;
    }
    mDragStartX = event->pos().x();
    mDragCurrentX = mDragStartX;
    mDragging = false;
    event->accept();
}

void HistogramWidget::mouseMoveEvent(QMouseEvent *event)
{
    const bool dragThresholdCrossedThisEvent = mDragStartX >= 0 &&
                                               (event->buttons() & Qt::LeftButton) != 0 &&
                                               std::abs(event->pos().x() - mDragStartX) > DRAG_THRESHOLD_PIXELS;
    // Suppress hover tooltips while the user is drag-brushing: they
    // are pointing at a range, not a bucket, so a single-bucket
    // popup is contradictory. Hide any tooltip left over from before
    // the drag threshold was crossed.
    if (mDragging || dragThresholdCrossedThisEvent)
    {
        QToolTip::hideText();
        mLastHoverBucket = -1;
    }
    else
    {
        UpdateHoverTooltip(event->pos());
    }
    if (mDragStartX >= 0 && (event->buttons() & Qt::LeftButton) != 0)
    {
        mDragCurrentX = event->pos().x();
        if (dragThresholdCrossedThisEvent)
        {
            mDragging = true;
        }
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void HistogramWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || mDragStartX < 0)
    {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const int releaseX = event->pos().x();
    const bool wasDragging = mDragging;
    mDragging = false;
    const int startX = mDragStartX;
    mDragStartX = -1;
    mDragCurrentX = -1;
    update();

    if (!wasDragging)
    {
        // Single click: emit `bucketClicked` for the first
        // non-empty bucket in the visual column under the click. The
        // visual-column mapping matters when `stride > 1`: mapping
        // through raw bucket coordinates would pick a bucket the
        // paint routine folded into an adjacent column, and the
        // table jump would land somewhere the user didn't visually
        // point at. Picking a non-empty bucket in the merged range
        // also keeps the jump useful — an empty leading bucket in a
        // stride>1 column would otherwise surface "no visible row"
        // even when the column obviously has bars.
        if (const auto columnOpt = VisualColumnAtX(releaseX); columnOpt.has_value())
        {
            emit bucketClicked(FirstNonEmptyBucketInColumn(*columnOpt));
        }
        event->accept();
        return;
    }

    // Drag release: convert brush endpoints to a time range. The
    // range covers *every* raw bucket inside the visual columns
    // between the two brush edges, so the installed filter matches
    // the bars the user actually swept across.
    const auto startColumn = VisualColumnAtX(std::min(startX, releaseX));
    const auto endColumn = VisualColumnAtX(std::max(startX, releaseX));
    if (!startColumn.has_value() || !endColumn.has_value() || mModel == nullptr)
    {
        event->accept();
        return;
    }
    const auto &idx = mModel->Index();
    const auto [startBegin, startEnd] = BucketRangeForVisualColumn(*startColumn);
    const auto [endBegin, endEnd] = BucketRangeForVisualColumn(*endColumn);
    if (startBegin >= startEnd || endBegin >= endEnd)
    {
        event->accept();
        return;
    }
    const auto fromUs = idx.BucketStart(startBegin).time_since_epoch().count();
    // `BucketEnd` is exclusive; subtract 1 us so the caller-side
    // inclusive comparator (`from <= ts && ts <= to`) doesn't spill
    // into the next bucket.
    const auto toUs = idx.BucketEnd(endEnd - 1).time_since_epoch().count() - 1;
    emit timeRangeSelected(fromUs, toUs);
    event->accept();
}

void HistogramWidget::wheelEvent(QWheelEvent *event)
{
    if ((event->modifiers() & Qt::ControlModifier) == 0)
    {
        QWidget::wheelEvent(event);
        return;
    }
    const int delta = event->angleDelta().y();
    if (delta > 0)
    {
        ZoomIn();
    }
    else if (delta < 0)
    {
        ZoomOut();
    }
    event->accept();
}

void HistogramWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape)
    {
        if (mDragging || mDragStartX >= 0)
        {
            CancelDrag();
            event->accept();
            return;
        }
    }
    if (event->key() == Qt::Key_Z)
    {
        if ((event->modifiers() & Qt::ShiftModifier) != 0)
        {
            ZoomOut();
        }
        else
        {
            ZoomIn();
        }
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

void HistogramWidget::contextMenuEvent(QContextMenuEvent *event)
{
    QMenu menu(this);
    QAction *resetAction = menu.addAction(tr("Reset zoom (auto)"));
    if (mModel != nullptr)
    {
        connect(resetAction, &QAction::triggered, this, [this]() {
            if (mModel != nullptr)
            {
                // `ResetBucketSizeToAuto` drops the manual-pin latch
                // first; a plain `ApplyAutoBucketSize` would be
                // silently vetoed after any Z / Shift+Z / Ctrl+wheel
                // zoom, leaving this menu entry inert. The subtitle
                // re-derives the rung label from the model on the
                // next paint, so no explicit refresh is needed here.
                mModel->ResetBucketSizeToAuto();
            }
        });
    }
    else
    {
        resetAction->setEnabled(false);
    }
    menu.exec(event->globalPos());
    event->accept();
}

void HistogramWidget::ZoomIn()
{
    if (mModel == nullptr)
    {
        return;
    }
    const auto current = static_cast<int>(mModel->Index().BucketSize());
    if (current == 0)
    {
        return;
    }
    mModel->SetBucketSize(static_cast<loglib::HistogramBucketSize>(current - 1));
    update();
}

void HistogramWidget::ZoomOut()
{
    if (mModel == nullptr)
    {
        return;
    }
    const auto current = static_cast<int>(mModel->Index().BucketSize());
    if (current + 1 >= static_cast<int>(loglib::HISTOGRAM_BUCKET_SIZE_COUNT))
    {
        return;
    }
    mModel->SetBucketSize(static_cast<loglib::HistogramBucketSize>(current + 1));
    update();
}

void HistogramWidget::CancelDrag()
{
    mDragging = false;
    mDragStartX = -1;
    mDragCurrentX = -1;
    // Hide any tooltip that was showing the hovered column: after
    // Esc the user wants a clean strip, not a lingering popup that
    // still references the range they abandoned.
    QToolTip::hideText();
    mLastHoverBucket = -1;
    update();
}

void HistogramWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    // OS-level palette / style / theme changes bypass `ThemeControl`
    // (on Windows a dark/light system switch arrives as
    // `QEvent::ThemeChange` without going through our theme
    // controller). Repaint on those so the bar palette stays in
    // sync with the surrounding chrome, and drop the tooltip cache
    // so the next hover recolours the popup. Mirrors the same idiom
    // `MainWindow::changeEvent` uses for the toolbar icons.
    if (event == nullptr)
    {
        return;
    }
    const auto type = event->type();
    if (type == QEvent::PaletteChange || type == QEvent::StyleChange || type == QEvent::ThemeChange)
    {
        mLastHoverBucket = -1;
        update();
    }
}
