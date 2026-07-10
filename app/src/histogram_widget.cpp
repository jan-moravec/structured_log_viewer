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

/// Bar stacking order, bottom to top. Higher severity on top so
/// error / fatal spikes dominate; `Unknown` at the bottom so unstyled
/// rows stay visible without hiding meaningful segments. The
/// `static_assert` below pins the size to catch a new canonical level
/// being added to `loglib` without an entry here.
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

/// Horizontal inset for `PlotRect` and `DetailsRect`. Wide enough
/// to keep the plot-area frame (drawn one pixel outside `PlotRect`)
/// clear of the widget's outer edge.
constexpr int PLOT_HORIZONTAL_PADDING = 4;

/// Vertical inset above the plot rect. Mirrors the horizontal padding.
constexpr int PLOT_INNER_PADDING = 4;

/// Height of the details strip below the bars. Doubles as the hover
/// readout (which replaced the old `QToolTip`).
constexpr int DETAILS_HEIGHT = 18;

/// Gap between the plot-area frame and the details strip so the
/// frame and text don't visually touch.
constexpr int DETAILS_TOP_PADDING = 4;

/// Minimum widget height. Preserves ~72 px of plot area after
/// reserving the bottom chrome.
constexpr int MIN_WIDGET_HEIGHT = 98;

/// Height of the anchor tick strip. Painted as an overlay at the top
/// of `PlotRect` rather than reserved chrome above it; reserving a
/// full row for ticks stole too much visual budget from the bars.
constexpr int ANCHOR_TICK_STRIP_HEIGHT = 10;

/// Alpha applied to each tick band. Lets the underlying bar colour
/// tint through so the tick reads as an overlay marker, not a bar slice.
constexpr int ANCHOR_TICK_ALPHA = 235;

/// Alpha for the drag-brush overlay. Subtle so bars stay readable.
constexpr int DRAG_BRUSH_ALPHA = 80;

/// Cosmetic pen width for the plot-area frame. Two pixels reads as
/// a defined chart border on both Hi-DPI and 100% displays.
constexpr int FRAME_PEN_WIDTH = 2;

/// Cosmetic pen width for per-bucket outlines. One pixel reads as a
/// subtle divider on wider bars; skipped on very narrow bars.
constexpr int BUCKET_PEN_WIDTH = 1;

/// Minimum bar width (px) at which we still draw the per-bucket
/// outline. Below this, a 1 px outline on each side leaves no visible
/// fill.
constexpr double MIN_BUCKET_OUTLINE_WIDTH_PX = 3.0;

/// Fallback colour for a level the active theme doesn't style.
/// Mirrors the row-styling fallback.
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

/// `date::format` template for @p size. Precision matches the bucket
/// width so a 1 h rung doesn't advertise fake sub-hour precision.
/// Aligned with the shape `log_table.cpp` uses for its `%F %T` cells.
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

/// Render @p ts as a local-time string using the same
/// `date::CurrentZone` / `date::format` pipeline the table uses.
/// Rounds to whole seconds (finest rung is 1 s).
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
/// A free helper so paint and hit-testing use the exact same layout.
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
        // Any data mutation invalidates the hover cache: the hovered
        // bucket may now have different counts, a new label, or be
        // gone entirely. Drop it so the details strip doesn't lie.
        connect(mModel, &HistogramModel::bucketsChanged, this, [this]() {
            mLastHoverBucket = -1;
            update();
        });
        connect(mModel, &HistogramModel::timeColumnAvailabilityChanged, this, [this](bool /*hasTime*/) {
            mLastHoverBucket = -1;
            update();
        });
        // Anchor changes leave bar geometry alone, so the hover cache
        // stays valid. Still needs a full `update()` because toggling
        // the strip's presence can change what `PlotRect` returns.
        connect(mModel, &HistogramModel::anchorBucketsChanged, this, [this]() { update(); });
    }
    if (mTheme != nullptr)
    {
        // Theme flip: bar palette and details-strip palette both
        // change. Drop the hover cache so the next mouse move
        // re-derives the readout under the new palette.
        connect(mTheme, &ThemeControl::themeChanged, this, [this]() {
            mLastHoverBucket = -1;
            update();
        });
    }
}

QRect HistogramWidget::PlotRect() const
{
    // The anchor tick strip is an overlay inside this rect, not
    // reserved chrome above it, so anchor-free and anchored sessions
    // share identical plot geometry (bars don't reflow when an
    // anchor is toggled). Bar heights normalise to `height()`.
    const int x = PLOT_HORIZONTAL_PADDING;
    const int y = PLOT_INNER_PADDING;
    const int w = std::max(0, width() - (2 * PLOT_HORIZONTAL_PADDING));
    const int bottomChrome = DETAILS_HEIGHT + DETAILS_TOP_PADDING;
    const int h = std::max(0, height() - PLOT_INNER_PADDING - bottomChrome);
    return {x, y, w, h};
}

QRect HistogramWidget::AnchorTickRect() const
{
    if (mModel == nullptr || !mModel->HasAnchorTicks())
    {
        return {};
    }
    const QRect plot = PlotRect();
    // Overlay at the top of `PlotRect`. Clamped by `plot.height()`
    // so a very short widget doesn't spill past the details line.
    const int stripHeight = std::min(ANCHOR_TICK_STRIP_HEIGHT, plot.height());
    return {plot.x(), plot.y(), plot.width(), stripHeight};
}

QRect HistogramWidget::DetailsRect() const
{
    const int stripHeight = DETAILS_HEIGHT;
    // Anchor to the bottom so a resize below `MIN_WIDGET_HEIGHT`
    // doesn't strand the strip in the middle of the plot area.
    const int y = std::max(0, height() - stripHeight);
    const int w = std::max(0, width() - (2 * PLOT_HORIZONTAL_PADDING));
    const int h = std::min(stripHeight, height());
    return {PLOT_HORIZONTAL_PADDING, y, w, h};
}

QString HistogramWidget::FormatDetailsLine() const
{
    if (mModel == nullptr)
    {
        return {};
    }
    const auto &idx = mModel->Index();
    const QLocale locale = QLocale::system();

    // Hovered-bucket branch: mirror the merged counts the paint
    // routine drew under the pointer. Falls through to the plot
    // summary if the hover cache went stale (column shrank away
    // between the mouse move and the paint).
    if (mLastHoverBucket >= 0 && !idx.Empty())
    {
        const auto columnIdx = static_cast<std::size_t>(mLastHoverBucket);
        const auto [begin, end] = BucketRangeForVisualColumn(columnIdx);
        if (begin < end)
        {
            const loglib::LevelBucket merged = MergeBuckets(idx.Buckets(), begin, end);
            const auto start = idx.BucketStart(begin);
            // `end` is exclusive: the span reaches the *end* of the
            // last merged bucket.
            const auto stop = idx.BucketEnd(end - 1);
            QString body = QStringLiteral("%1 \u2013 %2  \u00b7  total: %3")
                               .arg(FormatLocalTimestampForZoom(start, idx.BucketSize()))
                               .arg(FormatLocalTimestampForZoom(stop, idx.BucketSize()))
                               .arg(locale.toString(static_cast<qulonglong>(merged.Total())));
            for (const auto level : STACK_ORDER)
            {
                const uint32_t count = merged.counts[static_cast<std::size_t>(level)];
                if (count == 0)
                {
                    continue;
                }
                body.append(QStringLiteral("  \u00b7  "));
                body.append(CanonicalLevelNameQt(level));
                body.append(QStringLiteral(": "));
                body.append(locale.toString(static_cast<qulonglong>(count)));
            }
            return body;
        }
    }

    // Idle branch: plot summary. Also shown when the index is empty
    // so the strip explains what the bar area represents.
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
    return QStringLiteral("bucket: %1  \u00b7  rows: %2%3")
        .arg(bucketLabel)
        .arg(locale.toString(static_cast<qulonglong>(idx.TotalRowCount())))
        .arg(rangePart);
}

QString HistogramWidget::DetailsTextForTest() const
{
    return FormatDetailsLine();
}

HistogramModel::AnchorSlotMask HistogramWidget::VisualColumnAnchorMaskForTest(std::size_t col) const
{
    HistogramModel::AnchorSlotMask empty;
    if (mModel == nullptr)
    {
        return empty;
    }
    const auto anchorSlots = mModel->AnchorSlotsPerBucket();
    const std::size_t nBuckets = mModel->Index().Buckets().size();
    if (anchorSlots.empty() || nBuckets == 0)
    {
        return empty;
    }
    const QRect tickRect = AnchorTickRect();
    // Mirror the paint routine's layout so tests see the mask as it
    // would be painted. Empty tick rect (no anchor) -> empty mask.
    if (tickRect.width() <= 0)
    {
        return empty;
    }
    const VisualLayout layout = ComputeVisualLayout(nBuckets, tickRect.width());
    if (col >= layout.columnCount)
    {
        return empty;
    }
    const auto [begin, end] = BucketRangeForColumn(col, layout, nBuckets);
    HistogramModel::AnchorSlotMask merged;
    for (std::size_t i = begin; i < end && i < anchorSlots.size(); ++i)
    {
        merged |= anchorSlots[i];
    }
    return merged;
}

int HistogramWidget::AnchorTickStripHeightForTest() const
{
    return AnchorTickRect().height();
}

void HistogramWidget::PaintAnchorTickStrip(QPainter &painter)
{
    if (mModel == nullptr || !mModel->HasAnchorTicks() || mTheme == nullptr)
    {
        return;
    }
    const QRect tickRect = AnchorTickRect();
    if (tickRect.width() <= 0 || tickRect.height() <= 0)
    {
        return;
    }
    const auto anchorSlots = mModel->AnchorSlotsPerBucket();
    const std::size_t nBuckets = mModel->Index().Buckets().size();
    if (anchorSlots.empty() || nBuckets == 0)
    {
        return;
    }
    const VisualLayout tickLayout = ComputeVisualLayout(nBuckets, tickRect.width());
    for (std::size_t col = 0; col < tickLayout.columnCount; ++col)
    {
        const auto [begin, end] = BucketRangeForColumn(col, tickLayout, nBuckets);
        HistogramModel::AnchorSlotMask mergedMask;
        for (std::size_t i = begin; i < end && i < anchorSlots.size(); ++i)
        {
            mergedMask |= anchorSlots[i];
        }
        if (mergedMask.none())
        {
            continue;
        }
        // One sub-band per set slot (via `popcount`), so a bucket
        // with one slot fills the strip and a bucket with eight
        // slots crams eight bands into the same vertical budget.
        const std::size_t slotCount = mergedMask.count();
        const double columnX = tickRect.left() + (static_cast<double>(col) * tickLayout.columnWidth);
        const double columnWidth = std::max(1.0, tickLayout.columnWidth);
        const double bandHeight = static_cast<double>(tickRect.height()) / static_cast<double>(slotCount);
        std::size_t bandIndex = 0;
        for (std::size_t slot = 0; slot < mergedMask.size(); ++slot)
        {
            if (!mergedMask.test(slot))
            {
                continue;
            }
            const double bandTop = tickRect.top() + (static_cast<double>(bandIndex) * bandHeight);
            const QRectF band(columnX, bandTop, columnWidth, bandHeight);
            const QBrush brush = mTheme->AnchorBrushFor(static_cast<uint8_t>(slot), Qt::BackgroundRole);
            QColor color = brush.style() != Qt::NoBrush ? brush.color() : palette().color(QPalette::Highlight);
            // Slight alpha so the underlying bar tints through and
            // the tick reads as an overlay, not a bar slice.
            color.setAlpha(ANCHOR_TICK_ALPHA);
            painter.fillRect(band, color);
            ++bandIndex;
        }
    }
}

void HistogramWidget::paintEvent(QPaintEvent *event)
{
    // Even partial repaints (from `UpdateHoverState`) recompute the
    // plot: bars are cheap and the details rect is often included.
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const QRect plotRect = PlotRect();

    // Plot-area frame. Without it, dark level tints bleed into the
    // dock background and the strip reads as an ambiguous smear.
    // `QPalette::Mid` is Qt's semantic slot for subtle dividers and
    // contrasts against `Window` in both light and dark palettes.
    // Drawn one pixel outside `plotRect` (cosmetic pen) so it never
    // clips the bars. Drawn before the empty-state early-return so
    // the frame still tells the user where the histogram will appear.
    const QColor frameColor = palette().color(QPalette::Mid);
    if (plotRect.width() > 0 && plotRect.height() > 0)
    {
        painter.save();
        QPen framePen(frameColor);
        framePen.setCosmetic(true);
        framePen.setWidth(FRAME_PEN_WIDTH);
        painter.setPen(framePen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(plotRect.adjusted(-1, -1, 1, 1));
        painter.restore();
    }

    // Details strip below the bars. Drawn before any early return so
    // the readout is present in every state. Elided with font metrics
    // so narrow docks truncate cleanly instead of clipping mid-glyph.
    const QRect detailsRect = DetailsRect();
    if (detailsRect.width() > 0 && detailsRect.height() > 0)
    {
        painter.save();
        painter.setPen(palette().color(QPalette::WindowText));
        const QString detailsText = FormatDetailsLine();
        const QString elidedDetails = fontMetrics().elidedText(detailsText, Qt::ElideRight, detailsRect.width());
        painter.drawText(detailsRect, Qt::AlignVCenter | Qt::AlignLeft, elidedDetails);
        painter.restore();
    }

    // Empty-state placeholder: no model, no time column, empty
    // index, or degenerate plot rect.
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

    // Fold consecutive buckets into single visual columns when the
    // bar width would drop below 1 px. `stride` is 1 when every
    // bucket gets its own pixel.
    const VisualLayout layout = ComputeVisualLayout(nBuckets, plotWidth);
    if (layout.columnCount == 0)
    {
        return;
    }

    // Precompute merged totals so `maxTotal` reflects the tallest
    // *visible* column. Using raw bucket totals when `stride > 1`
    // would let aggregated columns overshoot the plot rect.
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

    // Fill pass: paint each column's stacked level segments. Adjacent
    // columns share their boundary pixel; the outline pass below
    // separates them cleanly.
    for (std::size_t col = 0; col < layout.columnCount; ++col)
    {
        const uint32_t total = merged[col].Total();
        if (total == 0)
        {
            continue;
        }
        const double columnX = plotRect.left() + (static_cast<double>(col) * layout.columnWidth);
        const double columnPixelWidth = std::max(1.0, layout.columnWidth);
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

    // Outline pass: trace each populated column with the frame colour.
    // Adjacent outlines rasterise onto the same pixel column, merging
    // into a single 1 px divider. Skipped on columns too narrow to
    // fit any fill next to a 1 px outline on each side.
    {
        painter.save();
        QPen bucketPen(frameColor);
        bucketPen.setCosmetic(true);
        bucketPen.setWidth(BUCKET_PEN_WIDTH);
        painter.setPen(bucketPen);
        painter.setBrush(Qt::NoBrush);
        for (std::size_t col = 0; col < layout.columnCount; ++col)
        {
            const uint32_t total = merged[col].Total();
            if (total == 0)
            {
                continue;
            }
            const double columnPixelWidth = std::max(1.0, layout.columnWidth);
            if (columnPixelWidth < MIN_BUCKET_OUTLINE_WIDTH_PX)
            {
                continue;
            }
            const double columnX = plotRect.left() + (static_cast<double>(col) * layout.columnWidth);
            const double totalHeight = plotHeight * (static_cast<double>(total) / static_cast<double>(maxTotal));
            const double stackTop = plotRect.bottom() + 1.0 - totalHeight;
            const QRectF columnRect(columnX, stackTop, columnPixelWidth, totalHeight);
            painter.drawRect(columnRect);
        }
        painter.restore();
    }

    // Anchor tick strip. Painted after the bars so ticks sit visibly
    // on top of any bar that reaches the plot's top edge.
    PaintAnchorTickStrip(painter);

    // Drag brush overlay. Fill and frame use the same `QRectF` so
    // the outline stays symmetric.
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
    // Every raw bucket empty. Return the column's first bucket so
    // the click still resolves; the caller then surfaces a "no
    // visible row" toast.
    return begin;
}

void HistogramWidget::UpdateHoverState(const QPoint &pos)
{
    if (mModel == nullptr)
    {
        if (mLastHoverBucket != -1)
        {
            mLastHoverBucket = -1;
            update(DetailsRect());
        }
        return;
    }
    const auto columnOpt = VisualColumnAtX(pos.x());
    if (!columnOpt.has_value())
    {
        if (mLastHoverBucket != -1)
        {
            mLastHoverBucket = -1;
            update(DetailsRect());
        }
        return;
    }
    const std::size_t columnIdx = *columnOpt;
    // Dedup: only repaint the details strip when the visual column
    // changes. Otherwise every mouse move inside the same bar would
    // repaint (flickers on some styles, burns CPU on fast drags).
    if (mLastHoverBucket >= 0 && std::cmp_equal(mLastHoverBucket, columnIdx))
    {
        return;
    }
    mLastHoverBucket = static_cast<int>(columnIdx);
    update(DetailsRect());
}

void HistogramWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        QWidget::mousePressEvent(event);
        return;
    }
    // Reject presses outside the plot rect (outer padding or the
    // details strip): the user hasn't pointed at a bar, so a
    // click/drag would jump the table or install a filter from an
    // accidental touch on the chrome.
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
    // While drag-brushing, the user is pointing at a range, not a
    // bucket -- suppress the single-bucket hover readout and let
    // the strip fall back to the plot summary.
    if (mDragging || dragThresholdCrossedThisEvent)
    {
        if (mLastHoverBucket != -1)
        {
            mLastHoverBucket = -1;
            update(DetailsRect());
        }
    }
    else
    {
        UpdateHoverState(event->pos());
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

void HistogramWidget::leaveEvent(QEvent *event)
{
    // Pointer left the widget: drop the hover cache so the details
    // strip snaps back to the plot summary.
    if (mLastHoverBucket != -1)
    {
        mLastHoverBucket = -1;
        update(DetailsRect());
    }
    QWidget::leaveEvent(event);
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
        // Single click: two outcomes, decided by where the click
        // landed:
        //  - Inside the tick strip on an anchored column ->
        //    `anchorClicked(sourceRow)` so the table lands on the
        //    anchor itself (not the bucket's first row).
        //  - Anywhere else (or tick-zone on an anchor-free column)
        //    -> `bucketClicked` with the first non-empty bucket in
        //    the visual column (so `stride > 1` clicks don't miss).
        if (const auto columnOpt = VisualColumnAtX(releaseX); columnOpt.has_value())
        {
            const QRect tickRect = AnchorTickRect();
            const bool clickedInTickStrip =
                tickRect.height() > 0 && tickRect.contains(event->pos()) && mModel != nullptr;
            if (clickedInTickStrip)
            {
                const auto [begin, end] = BucketRangeForVisualColumn(*columnOpt);
                if (begin < end)
                {
                    const int anchoredRow = mModel->FirstAnchoredRowInBucketRange(begin, end);
                    if (anchoredRow >= 0)
                    {
                        emit anchorClicked(anchoredRow);
                        event->accept();
                        return;
                    }
                }
            }
            emit bucketClicked(FirstNonEmptyBucketInColumn(*columnOpt));
        }
        event->accept();
        return;
    }

    // Drag release: convert brush endpoints to a time range covering
    // every raw bucket in the swept visual columns so the installed
    // filter matches the bars the user swept across.
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
    // `BucketEnd` is exclusive; subtract 1 us so the inclusive
    // comparator on the caller side doesn't spill into the next bucket.
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
                // `ResetBucketSizeToAuto` drops the manual pin first;
                // a plain `ApplyAutoBucketSize` would be vetoed after
                // any Z / Shift+Z / Ctrl+wheel zoom.
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
    // Drop the hover cache too so the details strip snaps to the
    // plot summary instead of lingering on a stale bucket.
    mLastHoverBucket = -1;
    update();
}

void HistogramWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    // OS-level palette / style / theme changes bypass `ThemeControl`
    // (e.g. Windows dark/light system switch arrives as
    // `QEvent::ThemeChange`). Repaint and drop the hover cache so
    // the details readout re-derives under the new palette. Same
    // idiom as `MainWindow::changeEvent` for toolbar icons.
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
