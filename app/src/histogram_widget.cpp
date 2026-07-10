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

/// Horizontal inset applied to `PlotRect` and `DetailsRect`. Also
/// leaves room for the outer plot-area frame -- the 2 px cosmetic
/// pen drawn one pixel outside `PlotRect` bleeds another ~1 px
/// beyond that, so anything smaller here risks clipping the frame
/// against the widget's outer edge.
constexpr int PLOT_HORIZONTAL_PADDING = 4;

/// Small vertical inset above the plot rect. Mirrors the horizontal
/// padding so the outer plot-area frame has room to breathe against
/// the widget's top edge.
constexpr int PLOT_INNER_PADDING = 4;

/// Height of the details strip painted below the bars. Matches the
/// visual weight the old top subtitle carried so the widget's
/// vertical rhythm doesn't change; the strip now doubles as the
/// hover readout that replaced `QToolTip`.
constexpr int DETAILS_HEIGHT = 18;

/// Vertical gap between the outer plot-area frame's bottom edge
/// and the details strip. Kept small but non-zero so the frame
/// and the text don't visually touch, especially when the text
/// contains a descender-heavy character right at the frame line.
constexpr int DETAILS_TOP_PADDING = 4;

/// Minimum widget height. Preserves the historical ~72 px plot
/// area even after reserving the new bottom chrome (details strip
/// + its top gap + the inner padding).
constexpr int MIN_WIDGET_HEIGHT = 98;

/// Height (in logical pixels) of the anchor tick strip. Painted as
/// an overlay at the top of `PlotRect` (over any bars that reach
/// that far), *not* as separate reserved chrome above the plot —
/// dedicating a full row for ticks stole too much visual budget
/// from the bars and made the ticks themselves feel cramped in
/// return. Tall enough to still resolve the per-slot vertical
/// sub-bands when multiple palette slots co-occur in the same
/// visual column.
constexpr int ANCHOR_TICK_STRIP_HEIGHT = 10;

/// Extra painter alpha applied to each tick band. Keeps the tick
/// legible when it overlays a filled bar segment underneath while
/// still letting the bar's colour tint through, so the reader can
/// tell the tick sits *on top of* the bar rather than replacing a
/// slice of it. Applied to the theme's `AnchorBrushFor` colour.
constexpr int ANCHOR_TICK_ALPHA = 235;

/// Alpha channel for the drag-brush overlay (0-255). Kept subtle so
/// the underlying bars stay readable through the highlight tint.
constexpr int DRAG_BRUSH_ALPHA = 80;

/// Cosmetic-pen width (device pixels) for the plot-area border.
/// Two pixels is enough to read as a defined "chart frame" on both
/// Hi-DPI and 100% displays without stealing visual weight from
/// the bars themselves.
constexpr int FRAME_PEN_WIDTH = 2;

/// Cosmetic-pen width (device pixels) for the per-bucket outlines.
/// Kept at one pixel so the outline reads as a subtle divider on
/// wider bars and can be safely skipped for bars too thin to
/// tolerate any outline (see `MIN_BUCKET_OUTLINE_WIDTH_PX`).
constexpr int BUCKET_PEN_WIDTH = 1;

/// Minimum on-screen bar width (logical pixels) at which we still
/// draw the per-bucket outline. Below this, a 1 px outline on each
/// side would leave zero or one pixel of fill visible, which loses
/// more information than the outline gains.
constexpr double MIN_BUCKET_OUTLINE_WIDTH_PX = 3.0;

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
        // Any data mutation invalidates the hover cache: the bucket
        // the pointer currently sits over may now report different
        // counts (streaming batch), a different label (rung change),
        // or not exist at all (retention eviction). Without this
        // reset the details strip would keep displaying a hover
        // readout for a bucket that no longer matches the paint.
        connect(mModel, &HistogramModel::bucketsChanged, this, [this]() {
            mLastHoverBucket = -1;
            update();
        });
        connect(mModel, &HistogramModel::timeColumnAvailabilityChanged, this, [this](bool /*hasTime*/) {
            mLastHoverBucket = -1;
            update();
        });
        // Anchor mask changes don't shift the bars (bucket geometry
        // is unchanged) so the hover cache stays valid. We still
        // need a repaint so the tick strip picks up the new mask.
        // Toggling the strip's presence also flips `PlotRect` (see
        // the `HasAnchorTicks` branch there), so a full `update()`
        // is required rather than just the tick rect.
        connect(mModel, &HistogramModel::anchorBucketsChanged, this, [this]() { update(); });
    }
    if (mTheme != nullptr)
    {
        // Same reasoning: a theme flip changes the bar palette and
        // the palette that colours the details strip; drop the
        // cached hover bucket so the next mouse move re-derives the
        // readout under the new palette.
        connect(mTheme, &ThemeControl::themeChanged, this, [this]() {
            mLastHoverBucket = -1;
            update();
        });
    }
}

QRect HistogramWidget::PlotRect() const
{
    // The plot area no longer reserves separate chrome for the
    // anchor tick strip: ticks are painted *inside* the plot area
    // at the top edge. That keeps the strip visually attached to
    // the bar the reader can see underneath, and it means a
    // no-anchor session and an anchored session share identical
    // plot geometry (the ticks overlap the bar tops rather than
    // resizing the bars). The bar-height scaling is unaffected
    // because bars are normalised against `PlotRect::height()`.
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
    // Overlay at the top of the plot rect: same x / width as bars
    // so ticks line up with the columns underneath. Height is
    // clamped by the plot rect so a very short widget doesn't
    // paint the strip past the details line.
    const int stripHeight = std::min(ANCHOR_TICK_STRIP_HEIGHT, plot.height());
    return {plot.x(), plot.y(), plot.width(), stripHeight};
}

QRect HistogramWidget::DetailsRect() const
{
    const int stripHeight = DETAILS_HEIGHT;
    // Anchor from the bottom so the widget can shrink below
    // `MIN_WIDGET_HEIGHT` (e.g. during a resize animation) without
    // stranding the strip in the middle of the plot area.
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

    // Hovered-bucket branch: use the visual column the pointer
    // last sat on, mirroring the merged counts the paint routine
    // actually rendered under the pointer. Falls through to the
    // plot summary when the hover cache is stale (the visual
    // column shrank away between the mouse move and the paint).
    if (mLastHoverBucket >= 0 && !idx.Empty())
    {
        const auto columnIdx = static_cast<std::size_t>(mLastHoverBucket);
        const auto [begin, end] = BucketRangeForVisualColumn(columnIdx);
        if (begin < end)
        {
            const loglib::LevelBucket merged = MergeBuckets(idx.Buckets(), begin, end);
            const auto start = idx.BucketStart(begin);
            // `end` is exclusive in raw-bucket coords; the visible
            // span ends at the *end* of the last merged bucket.
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

    // Idle branch: plot summary. Same content the old top subtitle
    // carried, kept here so an unloaded / empty index still tells
    // the user what the bar area is meant to represent.
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
    // Mirror the paint routine's layout: we want the mask *as it
    // would be painted*, folded through the same visual-column
    // stride the bar pass uses. When the tick strip isn't reserved
    // (no bucket carries an anchor) the mask is empty by construction.
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
        // Subdivide the strip vertically into one sub-band per set
        // slot. Using `popcount` avoids reserving pixels for slots
        // that aren't present, so a bucket with one slot fills the
        // strip and a bucket with eight slots crams eight thin
        // bands into the same vertical budget.
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
            // Slight alpha so the bar underneath tints through and
            // the reader can tell the tick is an overlay marker
            // rather than a bar segment.
            color.setAlpha(ANCHOR_TICK_ALPHA);
            painter.fillRect(band, color);
            ++bandIndex;
        }
    }
}

void HistogramWidget::paintEvent(QPaintEvent *event)
{
    // Partial repaints from `UpdateHoverState` request only the
    // details rect, but we still recompute the plot on every call
    // -- both because the details region can be `contains`-ed by
    // the paint event's rect and because the bars are cheap. The
    // parameter is required by the Qt override signature.
    Q_UNUSED(event);

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const QRect plotRect = PlotRect();

    // Outline the plot area. Without this the darker level tints
    // (info / debug on the dark themes especially) visually bleed
    // into the dock's background where they touch the frame edge,
    // and the strip reads as an ambiguous smear rather than a
    // bounded chart. `QPalette::Mid` is Qt's semantic slot for
    // subtle dividers -- contrasts against `Window` in both the
    // light and dark palettes, so we don't need theme-specific
    // wiring. The same colour is reused for the per-bucket outlines
    // below so the chart chrome reads as a single visual system.
    // Draw one pixel *outside* `plotRect` so we don't clip the
    // bars that fill it exactly, and keep the pen cosmetic so the
    // outline stays crisp on high-DPI displays. Drawn before the
    // empty-state early-return so the frame still tells the user
    // where the histogram will appear.
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

    // Details strip below the bars. Drawn before any early return
    // so the readout is present in every path -- empty state,
    // degenerate plot rect, and populated bars alike. Elide with
    // the current font metrics so a narrow dock (or a very wide
    // hovered-level breakdown) truncates cleanly instead of
    // clipping mid-glyph.
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
    // the plot rect and paint outside its bounds. The extra pass is
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

    // Fill pass: paint each column's stacked level segments. We used
    // to shave `0.5 px` off `columnPixelWidth` to leave an ad-hoc gap
    // between columns, but with a real per-bucket outline (the second
    // pass below) that gap became a fuzzy artefact rather than a
    // divider. Let adjacent columns share their boundary pixel and
    // rely on the outline to separate them.
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

    // Outline pass: trace each populated column with the same colour
    // as the plot-area frame. The shared right-edge of column N and
    // left-edge of column N+1 rasterise onto the same pixel column,
    // so adjacent outlines merge into a single 1 px divider (matching
    // the outer frame's aesthetic instead of the previous fuzzy gap).
    // Skipped for columns narrower than `MIN_BUCKET_OUTLINE_WIDTH_PX`
    // because a 1 px outline on each side would leave zero visible
    // fill, which loses more information than the divider adds.
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

    // Anchor tick strip overlay. Painted *after* the bars and their
    // outline pass so ticks sit visibly on top of any bar that
    // reaches the top of the plot area, and the reader can tell
    // the tick is a marker on the bar rather than a slice of it.
    PaintAnchorTickStrip(painter);

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
    // Dedup guard: the details strip only needs a repaint when
    // the visual column under the pointer changes. Without this
    // every mouse-move inside the same bar would repaint the
    // strip, which flickers on some styles and burns CPU during
    // fast drags across the widget.
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
    // Reject presses outside the plot rect (the outer padding and
    // the bottom details strip): the user hasn't pointed at a bar,
    // so treating this as a click (or the anchor of a drag) would
    // install a filter or jump the table from an accidental touch
    // on the details line.
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
    // Suppress the hover readout while the user is drag-brushing:
    // they are pointing at a range, not a bucket, so a single-bucket
    // details line is contradictory. Reset the cache and repaint
    // the strip so it falls back to the plot summary while the
    // drag is in flight.
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
    // The pointer left the widget entirely. Drop the hover cache
    // so the details strip snaps back to the plot summary and a
    // fresh entry over the same bar re-derives the hover readout.
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
        // Single click: two possible outcomes, decided by *where*
        // in the plot area the click landed.
        //
        //  - Inside the anchor tick strip AND the column carries an
        //    anchor: emit `anchorClicked(sourceRow)` so the table
        //    scrolls to the anchored row itself. Without this the
        //    reader sees the tick, clicks it, and lands on the
        //    bucket's first row — usually adjacent to but not the
        //    anchor they clicked, which is confusing.
        //  - Anywhere else (including a tick-zone click on a
        //    column with no anchor): fall through to the existing
        //    `bucketClicked` path.
        //
        // The visual-column mapping matters when `stride > 1`:
        // mapping through raw bucket coordinates would pick a
        // bucket the paint routine folded into an adjacent column,
        // and the table jump would land somewhere the user didn't
        // visually point at. Picking a non-empty bucket in the
        // merged range also keeps the jump useful — an empty
        // leading bucket in a stride>1 column would otherwise
        // surface "no visible row" even when the column obviously
        // has bars.
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
                // zoom, leaving this menu entry inert. The details
                // strip re-derives the rung label from the model
                // on the next paint, so no explicit refresh is
                // needed here.
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
    // After Esc the user wants a clean strip, so drop the hover
    // cache too -- the details line snaps back to the plot summary
    // instead of lingering on a bucket the pointer might no longer
    // be over by the time the drag is cancelled.
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
    // sync with the surrounding chrome, and drop the hover cache
    // so the next hover re-derives the details readout under the
    // new palette. Mirrors the same idiom `MainWindow::changeEvent`
    // uses for the toolbar icons.
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
