#include "overview_rail_widget.hpp"

#include "log_table_view.hpp"
#include "overview_rail_model.hpp"
#include "theme_control.hpp"

#include <loglib/log_level.hpp>
#include <loglib/theme.hpp>

#include <QAbstractItemView>
#include <QAbstractProxyModel>
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QEvent>
#include <QFontMetrics>
#include <QHideEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QPointF>
#include <QPointingDevice>
#include <QRect>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QSize>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QTimer>
#include <QWheelEvent>
#include <QWidget>
#include <Qt>

#include <QElapsedTimer>

#include <algorithm>
#include <array>
#include <bitset>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace
{

/// Vertical inset so tick outlines don't clip against the widget's
/// top / bottom edges.
constexpr int RAIL_VERTICAL_INSET = 2;

/// Horizontal inset for the level-underlay column. Steps in 1 px
/// from the wash so the two read as concentric bands.
constexpr int RAIL_UNDERLAY_INSET = 1;

/// Minimum bar width for a non-empty bucket; guarantees a single
/// row still paints as a visible tick.
constexpr int MIN_BAR_WIDTH_PX = 2;

/// Fallback alpha when the theme leaves a level unstyled.
/// Composites over `QPalette::Base` so Info-heavy rails don't
/// wash out.
constexpr int LEVEL_FALLBACK_ALPHA = 96;

/// Minimum width per severity segment. Keeps a rare level from
/// rounding to zero; 1 px so the floor doesn't manufacture a
/// bright band on the left edge.
constexpr int MIN_SEGMENT_WIDTH_PX = 1;

/// Alpha for the 1 px separator at the rail's left edge.
constexpr int SEPARATOR_ALPHA = 255;

/// Viewport-indicator fill alpha; overlay marker without erasing
/// the underlying wash.
constexpr int INDICATOR_FILL_ALPHA = 60;

/// Viewport-indicator outline alpha (`QPalette::Highlight`).
constexpr int INDICATOR_OUTLINE_ALPHA = 200;

/// Corner radius on the viewport indicator; matches the platform's
/// rounded-thumb look.
constexpr int INDICATOR_CORNER_RADIUS = 3;

/// Minimum indicator height so a tall session still has a hittable
/// target.
constexpr int INDICATOR_MIN_HEIGHT_PX = 12;

/// Minimum rail width in DP; hittable at small font sizes.
constexpr int RAIL_MIN_WIDTH_PX = 24;

/// Maximum rail width post-scaling. Sized for `Wide × 2` on
/// accessibility themes with a large `PM_ScrollBarExtent`.
constexpr int RAIL_MAX_WIDTH_PX = 128;

/// Scale factors per `OverviewRailWidthMode`.
constexpr double RAIL_WIDTH_SCALE_NARROW = 1.0;
constexpr double RAIL_WIDTH_SCALE_MEDIUM = 1.5;
constexpr double RAIL_WIDTH_SCALE_WIDE = 2.0;

/// Extra pixels on the DPI-fluent base so the rail doesn't visually
/// merge with the scrollbar.
constexpr int RAIL_WIDTH_PADDING = 2;

/// Trailing-edge debounce for `SyncBucketCountToHeight()` on
/// resize. Each `SetBucketCount(H)` reallocates buckets and,
/// with Find open, forces a full rescan; a per-pixel drag storm
/// would freeze the UI without coalescing. `showEvent` bypasses
/// the timer.
constexpr int BUCKET_SYNC_DEBOUNCE_MS = 100;

QColor ColorForLevel(const ThemeControl *theme, loglib::LogLevel level, const QPalette &palette)
{
    // Row *background* wash (not foreground): foreground is tuned
    // for contrast on top of this background and would read as a
    // bright pastel band on the rail. Makes the rail a mini-map
    // of the row-tinted table. `BackgroundFor` respects the
    // "high contrast levels" preference through the style cache.
    // Unstyled levels fall through to `QPalette::PlaceholderText`.
    if (theme != nullptr)
    {
        const QBrush brush = theme->BackgroundFor(level);
        if (brush.style() != Qt::NoBrush)
        {
            return brush.color();
        }
    }
    QColor c = palette.color(QPalette::PlaceholderText);
    c.setAlpha(LEVEL_FALLBACK_ALPHA);
    return c;
}

QColor ColorForAnchorSlot(const ThemeControl *theme, std::uint8_t colorIndex, const QPalette &palette)
{
    // Anchor colours come from `theme.anchorPalette`. Passing
    // `Qt::BackgroundRole` returns the slot fill; the wrong role
    // yields an invalid brush and collapses to `Highlight`.
    if (theme != nullptr)
    {
        const QBrush brush = theme->AnchorBrushFor(colorIndex, static_cast<int>(Qt::BackgroundRole));
        if (brush.style() != Qt::NoBrush)
        {
            return brush.color();
        }
    }
    return palette.color(QPalette::Highlight);
}

/// Log-scaled intensity in `[0, 1]` for a bucket with @p count
/// against rail-wide max @p maxCount. `log2(count + 1) /
/// log2(maxCount + 1)` keeps small bins visible next to hot
/// spots on power-law distributions (10 vs 10k reads ~30 %
/// instead of the linear 0.1 %). Returns 0 for an empty bucket.
[[nodiscard]] double IntensityForCount(std::uint32_t count, std::uint32_t maxCount) noexcept
{
    if (count == 0 || maxCount == 0)
    {
        return 0.0;
    }
    const double num = std::log2(static_cast<double>(count) + 1.0);
    const double den = std::log2(static_cast<double>(maxCount) + 1.0);
    if (den <= 0.0)
    {
        return 0.0;
    }
    return std::clamp(num / den, 0.0, 1.0);
}

/// Horizontal slot every paint pass draws into (bins, match
/// overlay, anchor overlay share it). Zero Y span; each bucket
/// fills its own slice.
[[nodiscard]] QRect ComputeContentRect(int underlayLeft, int underlayWidth) noexcept
{
    if (underlayWidth <= 0)
    {
        return {underlayLeft, 0, 0, 0};
    }
    return {underlayLeft, 0, underlayWidth, 0};
}

[[nodiscard]] double WidthScaleForMode(OverviewRailWidthMode mode) noexcept
{
    switch (mode)
    {
    case OverviewRailWidthMode::Narrow:
        return RAIL_WIDTH_SCALE_NARROW;
    case OverviewRailWidthMode::Wide:
        return RAIL_WIDTH_SCALE_WIDE;
    case OverviewRailWidthMode::Medium:
    default:
        return RAIL_WIDTH_SCALE_MEDIUM;
    }
}

} // namespace

OverviewRailWidthMode ParseOverviewRailWidthMode(const QString &value)
{
    if (value == QLatin1String("narrow"))
    {
        return OverviewRailWidthMode::Narrow;
    }
    if (value == QLatin1String("wide"))
    {
        return OverviewRailWidthMode::Wide;
    }
    // Default / unknown / "medium" → Medium (shipped default).
    return OverviewRailWidthMode::Medium;
}

QString OverviewRailWidthModeToSettingsString(OverviewRailWidthMode mode)
{
    switch (mode)
    {
    case OverviewRailWidthMode::Narrow:
        return QStringLiteral("narrow");
    case OverviewRailWidthMode::Wide:
        return QStringLiteral("wide");
    case OverviewRailWidthMode::Medium:
    default:
        return QStringLiteral("medium");
    }
}

OverviewRailWidget::OverviewRailWidget(
    OverviewRailModel *model, ThemeControl *theme, QAbstractItemView *tableView, QWidget *parent
)
    : QWidget(parent), mModel(model), mTheme(theme), mTableView(tableView)
{
    // Opaque `QPalette::Base` — same role the viewport and
    // histogram paint on. Level colours are tuned for this
    // background. `WA_OpaquePaintEvent` skips Qt's auto-fill;
    // `paintEvent` does its own fill to avoid scroll-drag
    // artefacts.
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(false);
    setCursor(Qt::ArrowCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setAutoFillBackground(true);
    setBackgroundRole(QPalette::Base);

    if (mModel != nullptr)
    {
        connect(mModel, &OverviewRailModel::bucketsChanged, this, qOverload<>(&QWidget::update));
        connect(mModel, &OverviewRailModel::matchesChanged, this, qOverload<>(&QWidget::update));
        connect(mModel, &OverviewRailModel::anchorBucketsChanged, this, qOverload<>(&QWidget::update));
    }

    if (mTableView != nullptr)
    {
        if (auto *vbar = mTableView->verticalScrollBar(); vbar != nullptr)
        {
            connect(vbar, &QAbstractSlider::valueChanged, this, qOverload<>(&QWidget::update));
            connect(vbar, &QAbstractSlider::rangeChanged, this, [this](int, int) { update(); });
        }
    }

    // Debounce for drag-resize storms; see `BUCKET_SYNC_DEBOUNCE_MS`.
    mBucketSyncTimer = new QTimer(this);
    mBucketSyncTimer->setObjectName(QStringLiteral("overviewRailBucketSyncTimer"));
    mBucketSyncTimer->setSingleShot(true);
    mBucketSyncTimer->setInterval(BUCKET_SYNC_DEBOUNCE_MS);
    connect(mBucketSyncTimer, &QTimer::timeout, this, &OverviewRailWidget::SyncBucketCountToHeight);
}

void OverviewRailWidget::SetWidthMode(OverviewRailWidthMode mode)
{
    if (mWidthMode == mode)
    {
        return;
    }
    mWidthMode = mode;
    mCachedRailWidth = 0;
    // Nudges the layout system so `LogTableView` re-reads
    // `sizeHint()` and refreshes the reserved right margin.
    updateGeometry();
    update();
}

QSize OverviewRailWidget::sizeHint() const
{
    // Base = `2 × max(PM_ScrollBarExtent, font 'M') + padding`,
    // floored at `RAIL_MIN_WIDTH_PX`. Width mode scales it, then
    // clamp into `[RAIL_MIN_WIDTH_PX, RAIL_MAX_WIDTH_PX]`.
    const QStyle *s = style();
    const int scrollbarExtent = (s != nullptr) ? s->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this) : 0;
    const int fontExtent = fontMetrics().horizontalAdvance(QLatin1Char('M'));
    const int base = std::max({2 * scrollbarExtent, 2 * fontExtent, RAIL_MIN_WIDTH_PX}) + RAIL_WIDTH_PADDING;
    const double scaled = static_cast<double>(base) * WidthScaleForMode(mWidthMode);
    const int extent = std::clamp(static_cast<int>(std::lround(scaled)), RAIL_MIN_WIDTH_PX, RAIL_MAX_WIDTH_PX);
    mCachedRailWidth = extent;
    return {extent, 0};
}

QSize OverviewRailWidget::minimumSizeHint() const
{
    return {RAIL_MIN_WIDTH_PX, 0};
}

int OverviewRailWidget::RailWidthForTest() const
{
    // Warm the cache for tests that ask before the first paint.
    if (mCachedRailWidth == 0)
    {
        (void)sizeHint();
    }
    return mCachedRailWidth;
}

QRect OverviewRailWidget::ViewportIndicatorRectForTest() const
{
    return ComputeViewportIndicatorRect();
}

int OverviewRailWidget::BucketAtYForTest(int y) const
{
    if (mModel == nullptr || mModel->BucketCount() == 0)
    {
        return -1;
    }
    const QRect rail = InteractiveRailRect();
    if (rail.isEmpty())
    {
        return -1;
    }
    const int clampedY = std::clamp(y - rail.top(), 0, rail.height() - 1);
    const std::size_t bucket =
        (static_cast<std::size_t>(clampedY) * mModel->BucketCount()) / static_cast<std::size_t>(rail.height());
    return static_cast<int>(std::min(bucket, mModel->BucketCount() - 1));
}

int OverviewRailWidget::YForBucketForTest(std::size_t bucket) const
{
    if (mModel == nullptr || mModel->BucketCount() == 0)
    {
        return -1;
    }
    const QRect rail = InteractiveRailRect();
    if (rail.isEmpty())
    {
        return -1;
    }
    const std::size_t nBuckets = mModel->BucketCount();
    const std::size_t clampedBucket = std::min(bucket, nBuckets - 1);
    const int y = rail.top() + static_cast<int>((clampedBucket * static_cast<std::size_t>(rail.height())) / nBuckets);
    return y;
}

int OverviewRailWidget::WidthForCountForTest(std::uint32_t count, std::uint32_t maxCount, int columnWidth)
{
    // Mirrors the paint pass. Empty bucket returns 0 so tests
    // can distinguish "no bar" from "min-width bar".
    if (columnWidth <= 0 || count == 0 || maxCount == 0)
    {
        return 0;
    }
    const double intensity = IntensityForCount(count, maxCount);
    const int barWidth = static_cast<int>(std::round(intensity * static_cast<double>(columnWidth)));
    return std::clamp(barWidth, MIN_BAR_WIDTH_PX, columnWidth);
}

QRect OverviewRailWidget::ContentRectForTest(int underlayLeft, int underlayWidth)
{
    return ComputeContentRect(underlayLeft, underlayWidth);
}

void OverviewRailWidget::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QPalette pal = palette();
    const QRect widgetRect = rect();

    // Explicit background fill: `WA_OpaquePaintEvent` disables
    // Qt's auto-fill; without this the previous paint smears
    // during scrollbar drags.
    painter.fillRect(widgetRect, pal.color(QPalette::Base));

    // 1 px `Dark` separator at the left edge so the boundary
    // against the viewport reads on any palette. Painted (not
    // stylesheet) so it survives style overrides.
    if (widgetRect.width() > 0)
    {
        QColor sepColor = pal.color(QPalette::Dark);
        sepColor.setAlpha(SEPARATOR_ALPHA);
        painter.fillRect(QRect(widgetRect.left(), widgetRect.top(), 1, widgetRect.height()), sepColor);
    }

    if (mModel == nullptr || mModel->BucketCount() == 0)
    {
        return;
    }

    const QRect rail = InteractiveRailRect();
    if (rail.isEmpty())
    {
        return;
    }

    const auto buckets = mModel->Buckets();
    const std::size_t nBuckets = buckets.size();
    const int railTop = rail.top();
    const int railHeight = rail.height();
    if (railHeight <= 0 || nBuckets == 0)
    {
        return;
    }

    const int underlayLeft = rail.left() + RAIL_UNDERLAY_INSET;
    const int underlayWidth = std::max(1, rail.width() - (2 * RAIL_UNDERLAY_INSET));
    const QColor highlightColor = pal.color(QPalette::Highlight);

    // Single content slot shared by bins + overlays. Paint order
    // anchors > matches > bins so a bucket with all three still
    // surfaces the user-set anchor.
    const QRect content = ComputeContentRect(underlayLeft, underlayWidth);
    const int contentLeft = content.left();
    const int contentWidth = content.width();

    // Precompute every bucket-edge Y once. Each paint pass reads
    // top from `yEdges[i]` and bottom from `yEdges[i+1]`, keeping
    // seams consistent across passes. Cached on
    // `(nBuckets, railTop, railHeight)` so drag-scroll bursts
    // skip the alloc.
    if (mCachedYEdgesBuckets != nBuckets || mCachedYEdgesRailTop != railTop || mCachedYEdgesRailHeight != railHeight)
    {
        mCachedYEdges.assign(nBuckets + 1, 0);
        for (std::size_t i = 0; i <= nBuckets; ++i)
        {
            mCachedYEdges[i] = railTop + static_cast<int>((i * static_cast<std::size_t>(railHeight)) / nBuckets);
        }
        mCachedYEdges.back() = railTop + railHeight;
        mCachedYEdgesBuckets = nBuckets;
        mCachedYEdgesRailTop = railTop;
        mCachedYEdgesRailHeight = railHeight;
    }
    const std::vector<int> &yEdges = mCachedYEdges;

    // Rail-wide max bucket total; drives the log-scaled bar
    // width in Pass 1. `nBuckets` ≈ 600, so recomputing per paint
    // is trivial and cheaper than plumbing a "maxChanged" signal.
    std::uint32_t maxCount = 0;
    std::size_t nonEmptyBuckets = 0;
    for (const auto &bucket : buckets)
    {
        const std::uint32_t total = bucket.levels.Total();
        maxCount = std::max(maxCount, total);
        if (total > 0)
        {
            ++nonEmptyBuckets;
        }
    }

    // Gated diagnostic: `LOGAPP_RAIL_TRACE=1` in the environment
    // (paired with `QT_FORCE_STDERR_LOGGING=1` on Windows) logs
    // the rail's paint state to `qInfo` at 1 Hz. Scaffolding for
    // future "invisible bars" regressions.
    static const bool RAIL_TRACE_ENABLED = qEnvironmentVariableIntValue("LOGAPP_RAIL_TRACE") != 0;
    constexpr int RAIL_TRACE_THROTTLE_MS = 1000;
    if (RAIL_TRACE_ENABLED)
    {
        static QElapsedTimer traceGate;
        if (!traceGate.isValid() || traceGate.hasExpired(RAIL_TRACE_THROTTLE_MS))
        {
            traceGate.restart();
            loglib::LogLevel firstDominant = loglib::LogLevel::Unknown;
            std::uint32_t firstBucketTotal = 0;
            QColor firstBarColor = pal.color(QPalette::Base);
            for (std::size_t i = 0; i < nBuckets; ++i)
            {
                if (buckets[i].levels.Total() > 0)
                {
                    firstBucketTotal = buckets[i].levels.Total();
                    firstDominant = mModel->DominantLevel(i);
                    firstBarColor = ColorForLevel(mTheme, firstDominant, pal);
                    break;
                }
            }
            const int proxyRows = mModel->ProxyRowCount();
            const int levelColIdx = mModel->LevelColumnIndexForTest();
            qInfo().noquote(
            ) << QStringLiteral("[rail-trace] widget=%1x%2 rail=%3x%4 content=%5 buckets=%6 non-empty=%7 "
                                "maxCount=%8 base=%9 firstBucket total=%10 dom=%11 color=%12 proxyRows=%13 "
                                "levelColIdx=%14")
                     .arg(widgetRect.width())
                     .arg(widgetRect.height())
                     .arg(rail.width())
                     .arg(rail.height())
                     .arg(contentWidth)
                     .arg(nBuckets)
                     .arg(nonEmptyBuckets)
                     .arg(maxCount)
                     .arg(pal.color(QPalette::Base).name())
                     .arg(firstBucketTotal)
                     .arg(static_cast<int>(firstDominant))
                     .arg(firstBarColor.name())
                     .arg(proxyRows)
                     .arg(levelColIdx);
        }
    }

    // Pass 1 (base): one bin bar per non-empty bucket, split into
    // severity segments in descending order (Fatal → Unknown).
    // Bar width = log-scaled total density; per-segment width =
    // that level's share of the bucket. Every non-zero level gets
    // `MIN_SEGMENT_WIDTH_PX` so a rare Fatal in a 500-Trace bucket
    // still lights a pixel.
    //
    // Match ticks (Pass 2) and anchor ticks (Pass 3) overlay this
    // base for buckets that carry them, so their signal wins over
    // the level painting.
    //
    // Hard-coded severity list rather than reading from the model
    // so the widget's paint has no cross-file coupling; grow this
    // alongside `LEVEL_SEVERITY_RANK` if the canonical list ever
    // grows.
    constexpr std::size_t SEVERITY_LEVEL_COUNT = loglib::CANONICAL_LEVEL_COUNT + 1;
    constexpr std::array<loglib::LogLevel, SEVERITY_LEVEL_COUNT> SEVERITY_DESCENDING{
        loglib::LogLevel::Fatal,
        loglib::LogLevel::Error,
        loglib::LogLevel::Warn,
        loglib::LogLevel::Info,
        loglib::LogLevel::Debug,
        loglib::LogLevel::Trace,
        loglib::LogLevel::Unknown,
    };
    if (contentWidth > 0)
    {
        for (std::size_t i = 0; i < nBuckets; ++i)
        {
            const auto &bucket = buckets[i];
            const std::uint32_t count = bucket.levels.Total();
            if (count == 0)
            {
                continue;
            }
            const int y = yEdges[i];
            const int height = std::max(1, yEdges[i + 1] - y);

            const double intensity = IntensityForCount(count, maxCount);
            int barWidth = static_cast<int>(std::round(intensity * static_cast<double>(contentWidth)));
            barWidth = std::clamp(barWidth, MIN_BAR_WIDTH_PX, contentWidth);

            // Two-pass segment sizing:
            //   1. Assign every non-zero level a
            //      `MIN_SEGMENT_WIDTH_PX` floor.
            //   2. Distribute the remainder proportionally to each
            //      level's `log2(count + 1)` weight.
            // Log weights compress the dynamic range so a rare
            // Fatal in a Trace-heavy bucket gets a readable slice
            // (~7 % vs 0.2 % linear) without erasing the majority.
            std::array<int, SEVERITY_LEVEL_COUNT> segWidths{};
            std::array<double, SEVERITY_LEVEL_COUNT> segWeights{};
            double totalWeight = 0.0;
            int reserved = 0;
            for (std::size_t s = 0; s < SEVERITY_DESCENDING.size(); ++s)
            {
                const auto levelIdx = static_cast<std::size_t>(SEVERITY_DESCENDING[s]);
                const std::uint32_t levelCount = bucket.levels.counts[levelIdx];
                if (levelCount > 0)
                {
                    segWidths[s] = MIN_SEGMENT_WIDTH_PX;
                    reserved += MIN_SEGMENT_WIDTH_PX;
                    segWeights[s] = std::log2(static_cast<double>(levelCount) + 1.0);
                    totalWeight += segWeights[s];
                }
            }
            const int extra = std::max(0, barWidth - reserved);
            if (extra > 0 && totalWeight > 0.0)
            {
                for (std::size_t s = 0; s < SEVERITY_DESCENDING.size(); ++s)
                {
                    if (segWeights[s] <= 0.0)
                    {
                        continue;
                    }
                    const int share = static_cast<int>((segWeights[s] / totalWeight) * static_cast<double>(extra));
                    segWidths[s] += share;
                }
                // Rounding leftovers go to the heaviest level so
                // the bar ends exactly at `barWidth` and the
                // dominant severity wins the visual tie-break.
                int allocated = 0;
                for (const int w : segWidths)
                {
                    allocated += w;
                }
                if (allocated < barWidth)
                {
                    std::size_t heaviest = 0;
                    double heaviestWeight = 0.0;
                    for (std::size_t s = 0; s < SEVERITY_DESCENDING.size(); ++s)
                    {
                        if (segWeights[s] > heaviestWeight)
                        {
                            heaviestWeight = segWeights[s];
                            heaviest = s;
                        }
                    }
                    if (heaviestWeight > 0.0)
                    {
                        segWidths[heaviest] += (barWidth - allocated);
                    }
                }
            }

            int xCursor = contentLeft;
            for (std::size_t s = 0; s < SEVERITY_DESCENDING.size(); ++s)
            {
                if (segWidths[s] == 0)
                {
                    continue;
                }
                const int w = std::min(segWidths[s], barWidth - (xCursor - contentLeft));
                if (w <= 0)
                {
                    break;
                }
                const QColor color = ColorForLevel(mTheme, SEVERITY_DESCENDING[s], pal);
                painter.fillRect(QRect(xCursor, y, w, height), color);
                xCursor += w;
            }
        }
    }

    // Pass 2 (overlay): match ticks. Repaint the full content
    // width for every bucket with at least one match so the
    // "there is a match here" signal is unambiguously visible.
    if (mModel->HasMatchTicks() && contentWidth > 0)
    {
        for (std::size_t i = 0; i < nBuckets; ++i)
        {
            if (buckets[i].matchCount == 0)
            {
                continue;
            }
            const int y = yEdges[i];
            const int height = std::max(1, yEdges[i + 1] - y);
            painter.fillRect(QRect(contentLeft, y, contentWidth, height), highlightColor);
        }
    }

    // Pass 3 (overlay): anchor ticks. Full content width for
    // every bucket with an anchor. Drawn after matches so
    // user-set markers outrank live search state.
    if (mModel->HasAnchorTicks() && contentWidth > 0)
    {
        for (std::size_t i = 0; i < nBuckets; ++i)
        {
            const auto &mask = buckets[i].anchorSlots;
            if (mask.none())
            {
                continue;
            }
            const int y = yEdges[i];
            const int height = std::max(1, yEdges[i + 1] - y);
            // Paint the lowest-index set slot: multiple anchor
            // colours in one bucket collapse to first-set
            // (deterministic; rare on a small rail).
            for (std::size_t s = 0; s < mask.size(); ++s)
            {
                if (mask.test(s))
                {
                    const QColor c = ColorForAnchorSlot(mTheme, static_cast<std::uint8_t>(s), pal);
                    painter.fillRect(QRect(contentLeft, y, contentWidth, height), c);
                    break;
                }
            }
        }
    }

    // Pass 4: viewport indicator. Rounded translucent fill with a
    // solid outline so the visible range reads as interactive
    // chrome over the level / tick painting.
    const QRect indicator = ComputeViewportIndicatorRect();
    if (!indicator.isEmpty())
    {
        painter.save();
        QColor fill = highlightColor;
        fill.setAlpha(INDICATOR_FILL_ALPHA);
        QColor outline = highlightColor;
        outline.setAlpha(INDICATOR_OUTLINE_ALPHA);
        painter.setBrush(fill);
        QPen pen(outline);
        pen.setCosmetic(true);
        pen.setWidth(1);
        painter.setPen(pen);
        // Half-pixel inset so the cosmetic pen lands on whole rows
        // and the rounded corners anti-alias cleanly.
        const QRectF rounded(
            indicator.left() + 0.5, indicator.top() + 0.5, indicator.width() - 1.0, indicator.height() - 1.0
        );
        painter.drawRoundedRect(rounded, INDICATOR_CORNER_RADIUS, INDICATOR_CORNER_RADIUS);
        painter.restore();
    }
}

void OverviewRailWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        QWidget::mousePressEvent(event);
        return;
    }
    mDragging = true;
    mLastEmittedRow = INT_MIN;
    // Fresh click commits: allow the handler to replace the
    // current selection with just this row (same idiom as the
    // histogram tick strip and anchors dock).
    EmitProxyRowForY(static_cast<int>(event->position().y()), /*replaceSelection=*/true);
    event->accept();
}

void OverviewRailWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!mDragging)
    {
        QWidget::mouseMoveEvent(event);
        return;
    }
    // Scrubbing: scroll without touching the selection so a
    // carefully-built multi-row selection survives a rail scrub.
    EmitProxyRowForY(static_cast<int>(event->position().y()), /*replaceSelection=*/false);
    event->accept();
}

void OverviewRailWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
    {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    mDragging = false;
    mLastEmittedRow = INT_MIN;
    event->accept();
}

void OverviewRailWidget::wheelEvent(QWheelEvent *event)
{
    // Swallow wheel during a drag so a fumbled scroll doesn't
    // fight the scrub. Otherwise forward to the table's
    // scrollbar so the rail behaves like an extension of it.
    if (mDragging)
    {
        event->accept();
        return;
    }
    if (mTableView == nullptr)
    {
        event->ignore();
        return;
    }
    auto *vbar = mTableView->verticalScrollBar();
    if (vbar == nullptr)
    {
        event->ignore();
        return;
    }
    // Translate local position into the scrollbar's coord space
    // before forwarding. `QAbstractSlider::wheelEvent` only reads
    // `angleDelta` today, but a future style / accessibility
    // layer inspecting the point would see coords outside `vbar`
    // without this. Global position stays as-is.
    //
    // Attribute the forthcoming `valueChanged` as user-driven:
    // `sendEvent` bypasses `LogTableView::wheelEvent` (the
    // normal attribution path), so we set the flag explicitly to
    // keep Follow-newest disengage correct even if a style /
    // platform path bypasses `triggerAction`.
    auto *logView = qobject_cast<LogTableView *>(mTableView.data());
    if (logView != nullptr)
    {
        logView->AttributeNextScrollToUser();
    }
    // Snapshot so we can detect the pinned-edge case: wheel
    // forwarded but scrollbar already at `minimum()` /
    // `maximum()`, so `valueChanged` never fires and the
    // attribution flag would survive until the next value change.
    const int valueBefore = vbar->value();
    const QPointF globalPos = event->globalPosition();
    const QPointF localPos = vbar->mapFromGlobal(globalPos.toPoint());
    QWheelEvent translated(
        localPos,
        globalPos,
        event->pixelDelta(),
        event->angleDelta(),
        event->buttons(),
        event->modifiers(),
        event->phase(),
        event->inverted(),
        event->source(),
        event->pointingDevice()
    );
    QApplication::sendEvent(vbar, &translated);
    // Pinned-edge cleanup: value didn't move, so
    // `OnVerticalScrollValueChanged` never fired and the flag we
    // set is stranded. Clear it so a later programmatic
    // `setValue` isn't misattributed as a user scroll.
    if (logView != nullptr && vbar->value() == valueBefore)
    {
        logView->ClearNextScrollUserAttribution();
    }
    event->accept();
}

void OverviewRailWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // Debounce: a window drag fires one resize per pixel of
    // height change; each `SetBucketCount(H±1)` drops the durable
    // per-bucket match counts and forces a full find rescan.
    // Coalescing collapses the burst to one update + one rescan
    // when the drag settles. Paints during the window use the
    // previous bucket count against the current height (slight
    // stretch, visually indistinguishable); hit-testing resolves
    // against the live widget height regardless.
    if (mBucketSyncTimer != nullptr)
    {
        mBucketSyncTimer->start();
    }
    else
    {
        SyncBucketCountToHeight();
    }
}

void OverviewRailWidget::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    // Cancel pending sync: `SetOverviewRailVisible(false)` drops
    // the model's bucket vector right after; a queued
    // `SyncBucketCountToHeight` would re-populate it and undo the
    // hidden-rail rebuild-cost optimisation.
    if (mBucketSyncTimer != nullptr)
    {
        mBucketSyncTimer->stop();
    }
}

void OverviewRailWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Re-arm the model after a hide-toggle dropped its bucket
    // vector. Skip when unsized (first show before geometry
    // runs) — `resizeEvent` supersedes with the real height.
    // Show is a discrete user action and must not wait on the
    // resize debounce; cancel any pending sync to avoid a
    // double-fire on the same H.
    if (height() > 0)
    {
        if (mBucketSyncTimer != nullptr)
        {
            mBucketSyncTimer->stop();
        }
        SyncBucketCountToHeight();
    }
}

void OverviewRailWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    // Style / palette / font changes shift DPI-fluent width and
    // the wash colour. Force a fresh sizeHint so the host view
    // re-reserves the right margin. `ScreenChangeInternal` acts
    // as a Qt 6.1+ proxy for DPR change; `DevicePixelRatioChange`
    // (Qt 6.6+) will pick up automatically when the floor rises.
    switch (event->type())
    {
    case QEvent::StyleChange:
    case QEvent::PaletteChange:
    case QEvent::FontChange:
    case QEvent::ApplicationFontChange:
    case QEvent::ScreenChangeInternal:
        updateGeometry();
        update();
        break;
    default:
        break;
    }
}

QRect OverviewRailWidget::InteractiveRailRect() const
{
    // Whole-file overview: rail top = first row, rail bottom =
    // last row, bars fill the widget height. Follows klogg /
    // VS Code minimap — vertical position is a linear projection
    // of file row index, not viewport Y. Restricting to viewport
    // Y was tried; users read it as "the rail is broken".
    QRect r = rect();
    r.adjust(0, RAIL_VERTICAL_INSET, 0, -RAIL_VERTICAL_INSET);
    return r;
}

QRect OverviewRailWidget::ComputeViewportIndicatorRect() const
{
    if (mTableView == nullptr || mModel == nullptr)
    {
        return {};
    }
    // The visible range is best expressed via `indexAt` on the
    // table (top + bottom of the viewport). `indexAt` already
    // tolerates a null model (returns an invalid index whose
    // `.row()` is `-1`), which the row-resolution branch below
    // treats the same as "empty layout".
    if (mTableView->model() == nullptr)
    {
        return {};
    }
    // Prefer the *live* row count from the attached table's model
    // over `mModel->ProxyRowCount()`. The latter is a snapshot
    // from the last rebuild -- between rebuilds during a heavy
    // stream it can lag the true count by a batch, and the
    // indicator would size against the stale value. Fall back to
    // the cached count when the live query returns 0 (rare, but
    // guards against a torn read during teardown).
    int totalRows = mTableView->model()->rowCount();
    if (totalRows <= 0)
    {
        totalRows = mModel->ProxyRowCount();
    }
    if (totalRows <= 0)
    {
        return {};
    }
    const QRect rail = InteractiveRailRect();
    if (rail.isEmpty())
    {
        return {};
    }
    const QRect viewport = mTableView->viewport()->rect();
    const int topRow = mTableView->indexAt(QPoint(0, 0)).row();
    // `indexAt(bottom)` returns -1 past the last row; fall back
    // to the row count so the indicator extends to the tail on
    // the last page.
    const int bottomIdxRow = mTableView->indexAt(QPoint(0, std::max(0, viewport.height() - 1))).row();
    int bottomRow = (bottomIdxRow >= 0) ? bottomIdxRow : (totalRows - 1);

    int visibleTop = (topRow >= 0) ? topRow : 0;
    bottomRow = std::max(bottomRow, visibleTop);
    // Clamp so the indicator can't run past the rail under a
    // transient row-count mismatch.
    visibleTop = std::clamp(visibleTop, 0, totalRows - 1);
    bottomRow = std::clamp(bottomRow, visibleTop, totalRows - 1);

    const int railHeight = rail.height();
    const long long yTop =
        (static_cast<long long>(visibleTop) * static_cast<long long>(railHeight)) / static_cast<long long>(totalRows);
    const long long yBottom = (static_cast<long long>(bottomRow + 1) * static_cast<long long>(railHeight)) /
                              static_cast<long long>(totalRows);
    const int naturalHeight = static_cast<int>(yBottom - yTop);
    const int indicatorHeight = std::max(naturalHeight, INDICATOR_MIN_HEIGHT_PX);
    // When the natural span is shorter than the min-height floor
    // (common on tall logs), expand around the centre of the
    // visible range so a mid-viewport row lines up with the
    // indicator's middle rather than its top edge.
    const long long yCenter = (yTop + yBottom) / 2;
    int indicatorTop = rail.top() + static_cast<int>(yCenter) - (indicatorHeight / 2);
    // Clamp so centring + min-height boost can't push the
    // indicator past either inset.
    if (indicatorTop + indicatorHeight > rail.bottom() + 1)
    {
        indicatorTop = rail.bottom() + 1 - indicatorHeight;
    }
    indicatorTop = std::max(indicatorTop, rail.top());

    return {rail.left(), indicatorTop, rail.width(), indicatorHeight};
}

void OverviewRailWidget::SyncBucketCountToHeight()
{
    if (mModel == nullptr)
    {
        return;
    }
    const QRect rail = InteractiveRailRect();
    const int height = std::max(0, rail.height());
    // One bucket per pixel row keeps the paint math trivial
    // (fillRect at each Y). `SetBucketCount` is a no-op on a
    // matching count.
    mModel->SetBucketCount(static_cast<std::size_t>(height));
}

#ifdef LOGAPP_BUILD_TESTING
void OverviewRailWidget::FlushPendingBucketSyncForTest()
{
    if (mBucketSyncTimer == nullptr || !mBucketSyncTimer->isActive())
    {
        return;
    }
    // Stop first so the timer's `timeout` can't race the manual
    // sync and double-fire.
    mBucketSyncTimer->stop();
    SyncBucketCountToHeight();
}
#endif

void OverviewRailWidget::EmitProxyRowForY(int y, bool replaceSelection)
{
    if (mModel == nullptr)
    {
        return;
    }
    const QRect rail = InteractiveRailRect();
    if (rail.isEmpty())
    {
        return;
    }
    const int relativeY = y - rail.top();
    const int proxyRow = mModel->ProxyRowForYPixel(relativeY, rail.height());
    if (proxyRow < 0 || proxyRow == mLastEmittedRow)
    {
        return;
    }
    mLastEmittedRow = proxyRow;
    emit proxyRowClicked(proxyRow, replaceSelection);
}
