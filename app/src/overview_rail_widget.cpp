#include "overview_rail_widget.hpp"

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
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QPen>
#include <QRect>
#include <QResizeEvent>
#include <QScrollBar>
#include <QShowEvent>
#include <QSize>
#include <QStyle>
#include <QStyleOptionViewItem>
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

/// Vertical pixel inset on the rail. Keeps tick outlines from
/// clipping against the widget's top / bottom edges and gives
/// the wash a visible frame line.
constexpr int RAIL_VERTICAL_INSET = 2;

/// Horizontal pixel inset for the level-underlay column. The
/// wash background paints edge-to-edge; the coloured underlay
/// steps in 1 px so the two reads as concentric bands rather
/// than a single flat fill.
constexpr int RAIL_UNDERLAY_INSET = 1;

/// Minimum bar width for a non-empty level bucket. Guarantees a
/// 1-row bucket still paints as a visible 2-px tick, so sparse
/// activity zones don't visually vanish in a mostly-empty log.
constexpr int MIN_BAR_WIDTH_PX = 2;

/// Alpha applied to the *fallback* segment colour when the
/// theme leaves a level unstyled (see `ColorForLevel`). Keeps
/// Info-heavy rails from washing out by compositing the
/// neutral placeholder-text tone semi-transparently over
/// `QPalette::Base`. Styled levels (Fatal, Error, Warn, ...)
/// paint fully opaque and take their colour from the theme's
/// row *background* which is designed as a subtle severity
/// tint on Base -- see `ColorForLevel` for the rationale.
constexpr int LEVEL_FALLBACK_ALPHA = 96;

/// Minimum horizontal width for one severity segment inside a
/// bucket's bar. Keeps a rare-but-present level (e.g. one Fatal
/// in a 500-row bucket) from rounding away to zero pixels. Kept
/// tiny (1 px) so the floor doesn't manufacture a bright band
/// on the rail's left edge -- combined with the dark row-
/// background palette in `ColorForLevel`, even every-bucket
/// severity segments composite to a subtle tint, not a stripe.
constexpr int MIN_SEGMENT_WIDTH_PX = 1;

/// Alpha on the 1 px separator line drawn at the rail's left
/// edge. Gives the rail a hard boundary against the table body
/// so the eye can find it even when the palette Window colour
/// is close to Base. `QPalette::Dark` (window's shadow tone)
/// reads as a divider on both Light and Dark themes.
constexpr int SEPARATOR_ALPHA = 255;

/// Alpha on the viewport-indicator glass fill. Higher than
/// the wash so the indicator reads as an overlay marker;
/// lower than opaque so the wash still shows through.
constexpr int INDICATOR_FILL_ALPHA = 60;

/// Alpha on the viewport-indicator outline. Solid pen with
/// palette Highlight so the indicator reads as focused UI
/// chrome, not a decoration.
constexpr int INDICATOR_OUTLINE_ALPHA = 200;

/// Corner radius (device-independent px) on the viewport
/// indicator. Matches the platform's rounded-thumb aesthetic
/// without pretending to be the actual scrollbar thumb.
constexpr int INDICATOR_CORNER_RADIUS = 3;

/// Minimum viewport-indicator height. Ensures a very-tall
/// session still exposes a hittable indicator; without this a
/// 1 M row session would collapse the indicator to sub-pixel.
constexpr int INDICATOR_MIN_HEIGHT_PX = 12;

/// Minimum rail width in device-independent px. Guarantees a
/// hittable target at very small font sizes; the single-column
/// bin section still stays readable at this width because the
/// stacked severity segments compress down to 1 px per level
/// (which is enough to spot a rare Fatal streak).
constexpr int RAIL_MIN_WIDTH_PX = 24;

/// Maximum rail width in device-independent px. Some styles
/// (accessibility themes on Windows, GTK "chunky-scrollbar"
/// setups) advertise very large `PM_ScrollBarExtent` values —
/// mirroring them 1:1 would cover a large fraction of the
/// viewport with a nearly-empty rail. The cap is deliberately
/// wide so per-level stacked-segment colours are
/// unmistakably readable even on 4K displays where 14 px bars
/// were near-invisible in user reports. Klogg's overview strip
/// is 16 px; we run wider because we also show anchors + match
/// ticks in dedicated columns, not just matches.
constexpr int RAIL_MAX_WIDTH_PX = 60;

/// Extra width added to the DPI-fluent metric. Nudges the
/// rail slightly wider than the scrollbar so it doesn't merge
/// visually with a scrollbar rendered in the same theme.
constexpr int RAIL_WIDTH_PADDING = 2;

QColor ColorForLevel(const ThemeControl *theme, loglib::LogLevel level, const QPalette &palette)
{
    // Rail bars use the active theme's row *background* wash
    // rather than its row *foreground* text tone. Foregrounds
    // (Warn `#FCD34D`, Error `#FCA5A5`, Fatal `#FECACA` on Dark)
    // are tuned for legible contrast against the row background
    // and read as *bright pastels* when painted directly on Base
    // -- multiple such bright tones stacked across the rail then
    // composite to a washed-out "light band" (the regression the
    // "rail is still too light" report was about). Row
    // backgrounds are the opposite: theme designers pick them as
    // *subtle* severity tints layered on Base, so a rail bin
    // painted with a level's row background matches exactly what
    // the eye sees on the corresponding table row -- a mini-map
    // of the file's severity pattern.
    //
    // `ThemeControl::BackgroundFor` reads from `BuildStyleCache`,
    // which resolves per-level styles through
    // `StyleForLevel(theme, level, mHighContrast)` -- so this
    // call automatically picks up the theme's
    // `levelsHighContrast` block when the user has the
    // Preferences "high contrast levels" checkbox on. No extra
    // wiring is needed here; toggling the setting rebuilds the
    // cache and repaints the rail with the loud variants.
    //
    // Themes intentionally leave some levels unstyled (built-in
    // Dark / Light leave `Info` blank so Info rows read as
    // ordinary chrome). For those, fall back to
    // `QPalette::PlaceholderText` composited at
    // `LEVEL_FALLBACK_ALPHA` -- a dim theme-driven grey that
    // keeps the rail readable without overwhelming it when the
    // unstyled level dominates the bucket totals.
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
    // Anchor colours come from the theme's per-slot anchor
    // palette (`theme.anchorPalette` in each JSON, resolved by
    // `ThemeControl::AnchorBrushFor`). The API accepts either
    // `Qt::BackgroundRole` (slot fill) or `Qt::ForegroundRole`
    // (contrast text); every other caller in the app --
    // `log_model.cpp` (row rendering), `histogram_widget.cpp`,
    // `anchors_dock.cpp`, and `main_window.cpp` -- passes
    // `Qt::BackgroundRole` to get the anchor's fill colour, and
    // the rail must do the same or `AnchorBrushFor` returns an
    // invalid brush and every anchor collapses to the fallback
    // `QPalette::Highlight` regardless of slot. Bug repro:
    // set three anchors on different palette slots and observe
    // the rail paint them all in the same accent colour.
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

/// Log-scaled intensity in [0, 1] for a bucket with @p count
/// rows against a rail-wide max of @p maxCount. Uses
/// `log2(count + 1) / log2(maxCount + 1)` so a 1-row bucket
/// still returns a positive intensity (no `log2(0)` trap) and a
/// max-count bucket saturates to 1.0. The log compresses the
/// dynamic range so small bins stay visible next to hot spots
/// on power-law distributions -- a bucket with 10 rows against
/// a max of 10k still gets ~30 % of the level column instead of
/// the ~0.1 % a linear map would give it. Log base is irrelevant
/// to the shape; `log2` chosen for readability. Returns 0 for
/// an empty bucket, and clamps to `[0, 1]` for defence.
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

/// Single content rect: the horizontal slot the paint passes
/// draw into. Reflects the widget's move from a three-column
/// layout (level / match / anchor side-by-side) to a single
/// full-width bin section that match and anchor ticks *overlay*
/// on top of. Returned with a zero Y span; each bucket's paint
/// pass fills its own Y slice.
[[nodiscard]] QRect ComputeContentRect(int underlayLeft, int underlayWidth) noexcept
{
    if (underlayWidth <= 0)
    {
        return QRect(underlayLeft, 0, 0, 0);
    }
    return QRect(underlayLeft, 0, underlayWidth, 0);
}

} // namespace

OverviewRailWidget::OverviewRailWidget(
    OverviewRailModel *model, ThemeControl *theme, QAbstractItemView *tableView, QWidget *parent
)
    : QWidget(parent), mModel(model), mTheme(theme), mTableView(tableView)
{
    // Opaque `QPalette::Base` background -- the same role the
    // viewport and the histogram widget paint their content on.
    // On Dark Fusion (Base #232629 vs Window #31363B) this makes
    // the rail read as a *narrow extension of the table's data
    // area*, not as chrome next to the scrollbar. The practical
    // win is contrast: the per-level colours the theme returns
    // via `ColorForLevel` are tuned for legibility on Base
    // (that's where the row backgrounds live), so Info-heavy
    // buckets paint a visible blue instead of a Window-blended
    // ghost. `WA_OpaquePaintEvent` tells Qt we cover every
    // pixel opaquely so it can skip the auto-fill call before
    // our paintEvent -- combined with the explicit background
    // fill at the top of paintEvent this keeps scroll-drag
    // repaints artefact-free.
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
}

QSize OverviewRailWidget::sizeHint() const
{
    // DPI-fluent width: anchor to twice the platform scrollbar
    // extent so the rail scales with the same DPI metric users
    // already recognise, but stays wide enough that the three
    // internal columns (level / match / anchor) each get several
    // clearly visible pixels. Fall back to the font's `M` advance
    // when the style returns a degenerate 0 (offscreen QPA in
    // tests). Clamp into `[RAIL_MIN_WIDTH_PX, RAIL_MAX_WIDTH_PX]`
    // so a themed style with a giant scrollbar extent doesn't
    // inflate the rail past the point where extra pixels stop
    // conveying information, and a tiny extent (accessibility
    // narrow-scrollbar setups) can't collapse the level column
    // to a single-pixel streak.
    //
    // The 2x factor was tuned against the "invisible bars"
    // report on Windows 11: a raw scrollbar extent of 17 px
    // collapsed the level column to ~10 px, which reads as a
    // hair-thin line at typical viewing distance. Doubling puts
    // the level column at ~19 px -- comfortably readable per
    // level colour.
    const QStyle *s = style();
    const int scrollbarExtent = (s != nullptr) ? s->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this) : 0;
    const int fontExtent = fontMetrics().horizontalAdvance(QLatin1Char('M'));
    const int raw = std::max({2 * scrollbarExtent, 2 * fontExtent, RAIL_MIN_WIDTH_PX}) + RAIL_WIDTH_PADDING;
    const int extent = std::clamp(raw, RAIL_MIN_WIDTH_PX, RAIL_MAX_WIDTH_PX);
    // Cache so `RailWidthForTest` (and any future non-const
    // helper that wants the last DPI-fluent width) doesn't have
    // to re-run the platform-metric query.
    mCachedRailWidth = extent;
    return {extent, 0};
}

QSize OverviewRailWidget::minimumSizeHint() const
{
    return {RAIL_MIN_WIDTH_PX, 0};
}

int OverviewRailWidget::RailWidthForTest() const
{
    // Warm the cache when a test asks before the first
    // `sizeHint()` roundtrip, then return the cached value.
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
    const std::size_t bucket = (static_cast<std::size_t>(clampedY) * mModel->BucketCount()) /
                               static_cast<std::size_t>(rail.height());
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
    const int y = rail.top() + static_cast<int>(
                                   (clampedBucket * static_cast<std::size_t>(rail.height())) / nBuckets
                               );
    return y;
}

int OverviewRailWidget::WidthForCountForTest(std::uint32_t count, std::uint32_t maxCount, int columnWidth)
{
    // Mirrors the paint pass: clamp column width to non-negative,
    // scale by the log-based intensity, then clamp into
    // `[MIN_BAR_WIDTH_PX, columnWidth]`. An empty bucket returns
    // zero so tests can distinguish "no bar" from "min-width bar".
    if (columnWidth <= 0 || count == 0 || maxCount == 0)
    {
        return 0;
    }
    const double intensity = IntensityForCount(count, maxCount);
    int barWidth = static_cast<int>(std::round(intensity * static_cast<double>(columnWidth)));
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

    // Fully repaint the widget's background on every paint pass.
    // Qt::WA_OpaquePaintEvent (set in the ctor) tells Qt we cover
    // every pixel, which *disables* `autoFillBackground` -- so
    // without this explicit fill the previous paint's content
    // would leak through (visible as smearing / trails when the
    // user drags the scrollbar and successive paints don't fully
    // overwrite the old level bar / viewport indicator paint).
    // Using `QPalette::Base` matches the viewport's / histogram's
    // background tone so the level bar colours (which are tuned
    // for legibility on Base) show at the expected contrast.
    painter.fillRect(widgetRect, pal.color(QPalette::Base));

    // Draw a 1 px `QPalette::Dark` separator at the left edge
    // so the rail's boundary against the viewport is crisp
    // regardless of how close Window and Base ended up under
    // the user's palette. Kept as a painter line rather than
    // a stylesheet border so it survives style overrides that
    // strip the widget's frame (Windows 11 Fusion, some
    // accessibility themes).
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

    // Single content slot: the whole underlay. Match and anchor
    // ticks *overlay* the level bins in the same slot (previously
    // each channel got its own sub-column, which fractured a
    // small rail into three near-invisible strips). Paint order
    // guarantees anchors > matches > bins so a bucket that
    // carries all three still surfaces the user-set anchor.
    const QRect content = ComputeContentRect(underlayLeft, underlayWidth);
    const int contentLeft = content.left();
    const int contentWidth = content.width();

    // Precompute the Y coordinate of every bucket edge once.
    // Each of the paint passes below picks the top from
    // `yEdges[i]` and the bottom from `yEdges[i+1]` -- avoids
    // recomputing the same integer division per bucket on tall
    // rails and keeps the seam-rounding ("bottom == next
    // bucket's top") consistent across passes.
    std::vector<int> yEdges(nBuckets + 1);
    for (std::size_t i = 0; i <= nBuckets; ++i)
    {
        yEdges[i] = railTop + static_cast<int>((i * static_cast<std::size_t>(railHeight)) / nBuckets);
    }
    yEdges.back() = railTop + railHeight;

    // Rail-wide max bucket total. Drives the log-scaled bar
    // width in Pass 1 so density is expressed as bar length
    // relative to the busiest bucket. Computed in one linear
    // walk over `buckets` (nBuckets = railHeight ~= 600, so
    // trivial); recomputed on every paint because the model
    // has no rail-wide max cache and the alternative (subscribe
    // to a "maxChanged" signal) buys us microseconds we don't
    // need.
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

    // Gated diagnostic: with `LOGAPP_RAIL_TRACE=1` in the
    // environment (paired with `QT_FORCE_STDERR_LOGGING=1` on
    // Windows so the GUI subsystem output routes to stderr),
    // log the rail's paint state to `qInfo`. Rate-limited to
    // once per second so drag-repaint bursts don't flood the
    // console. This is scaffolding for future "invisible bars"
    // regressions -- see run_diag.ps1 for the end-to-end reproducer
    // (launch the app, capture the window via PrintWindow, parse
    // pixels for expected colours).
    static const bool RAIL_TRACE_ENABLED = qEnvironmentVariableIntValue("LOGAPP_RAIL_TRACE") != 0;
    // 1 Hz throttle window: fast enough to see live buckets fill in
    // during streaming, slow enough that drag-repaint bursts don't
    // flood the console with duplicate lines.
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
            qInfo().noquote() << QStringLiteral(
                                     "[rail-trace] widget=%1x%2 rail=%3x%4 content=%5 buckets=%6 non-empty=%7 "
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

    // Pass 1 (base layer): one bin bar per non-empty bucket,
    // split into a stack of severity segments (Fatal / Error /
    // Warn / Info / Debug / Trace / Unknown left-to-right in
    // severity-descending order). Bar *width* encodes total row
    // density (log-scaled -- small bins still show a visible
    // tick, hot spots saturate to the full content width); each
    // segment's width inside the bar is proportional to that
    // level's share of the bucket.
    //
    // Every non-zero level gets `MIN_SEGMENT_WIDTH_PX` (1 px) so
    // a rare Fatal in a 500-Trace bucket still lights up a
    // pixel. This was previously the source of a "left-edge
    // bright stripe" regression when the paint used row
    // *foreground* colours (Warn `#FCD34D`, Fatal `#FECACA`,
    // ...) -- one bright pixel per bucket became a continuous
    // vertical light band on real logs. `ColorForLevel` now
    // resolves to the row *background* wash instead, which is a
    // subtle severity tint on Base, so even every-bucket
    // segments composite to a soft mini-map rather than a
    // stripe. `BackgroundFor` also respects
    // `mHighContrast` automatically (via `BuildStyleCache`), so
    // toggling the Preferences "high contrast levels" checkbox
    // recolours the rail with the loud
    // `levelsHighContrast` variants without any extra plumbing.
    //
    // Match ticks (Pass 2) and anchor ticks (Pass 3) later
    // *overlay* this base layer for buckets that carry them, so
    // the "you have a match here" / "you have an anchor here"
    // signal wins for hit-testing purposes even though the base
    // layer painted the same slot first.
    //
    // Kept as a hard-coded severity-descending list (rather than
    // reaching into the model's `LEVEL_SEVERITY_RANK`) so the
    // widget's paint pass has no cross-file coupling. If the
    // canonical level list ever grows the initialiser has to
    // grow alongside `LEVEL_SEVERITY_RANK` in the model or the
    // paint pass will silently miss the new level.
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
            //   1) assign every non-zero level a
            //      `MIN_SEGMENT_WIDTH_PX` floor and total the
            //      reserved pixels;
            //   2) distribute the remaining `barWidth -
            //      reserved` proportionally to each level's
            //      *log-weighted* share of the bucket.
            //
            // Weights use `log2(count + 1)` (the same
            // transform `IntensityForCount` applies to the
            // bar's total density). Linear proportions
            // effectively hide rare severities: a bucket of
            // 500 Trace + 1 Fatal gives Fatal 1/501 = 0.2 % of
            // the bar, floored to a single pixel that reads
            // as noise on a Trace-dominated rail. Log-weighted
            // shares raise that Fatal slice to
            // log2(2) / (log2(501) + log2(2)) ~= 7 %, which is
            // a visibly readable band without letting the
            // majority level lose its "this is the dominant
            // one" cue. Both scales point in the same
            // direction (bigger count -> bigger slice), just
            // with a compressed dynamic range that matches
            // human contrast sensitivity better.
            std::array<int, SEVERITY_LEVEL_COUNT> segWidths{};
            std::array<double, SEVERITY_LEVEL_COUNT> segWeights{};
            double totalWeight = 0.0;
            int reserved = 0;
            for (std::size_t s = 0; s < SEVERITY_DESCENDING.size(); ++s)
            {
                const std::size_t levelIdx = static_cast<std::size_t>(SEVERITY_DESCENDING[s]);
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
                    const int share =
                        static_cast<int>((segWeights[s] / totalWeight) * static_cast<double>(extra));
                    segWidths[s] += share;
                }
                // Rounding leftovers: hand them to the level
                // that carries the largest weight (the
                // majority in log space), so the bar's right
                // edge lands on `barWidth` exactly and the
                // dominant severity gets the visual tie-break.
                int allocated = 0;
                for (int w : segWidths)
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

    // Pass 2 (overlay): match ticks. Repaint the *entire content
    // width* for every bucket that carries at least one match
    // row so the "there is a match here" signal is a solid,
    // high-contrast Highlight-colour bar that unambiguously
    // wins over the base-layer bin painting. Full-width instead
    // of an accent column so a search hit in an otherwise-quiet
    // bucket still catches the eye at a glance.
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

    // Pass 3 (overlay): anchor ticks. Repaint the *entire content
    // width* for every bucket that carries at least one anchor
    // row. Painted after matches so a bucket that is both an
    // anchor and a search hit reads as an anchor -- user-set
    // markers outrank live search state. Anchor colour comes
    // from `ThemeControl::AnchorBrushFor` (theme's
    // `anchorPalette`).
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
            // colours in one bucket collapse to the first-set
            // (deterministic, and rare for a small rail).
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

    // Pass 4: viewport indicator. Rounded, translucent fill
    // with a solid outline so the visible range reads as
    // interactive chrome over the level / tick painting.
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
        // Inset by 0.5 px so the cosmetic pen lands on a whole
        // pixel row and the rounded corners anti-alias cleanly.
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
    // A fresh click commits the user to that row: the downstream
    // handler is allowed to replace the current selection with
    // just this row (matches the "click to jump" idiom used by
    // the histogram tick strip and the anchors dock).
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
    // Scrubbing: the user is exploring, not committing. Ask the
    // handler to scroll without touching the selection so a
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
    // While actively dragging, swallow the wheel so a fumbled
    // scroll doesn't fight the drag scrub. Otherwise forward
    // by injecting a matching delta into the table's scrollbar
    // so the rail behaves like an extension of the scrollbar.
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
    // TODO: `event->position()` is in this widget's coordinate
    // system, not the scrollbar's. `QAbstractSlider::wheelEvent`
    // only reads `angleDelta` today so the mismatch is
    // invisible, but any style / accessibility layer that
    // inspects position sees rail-widget coordinates. Consider
    // translating with `QWheelEvent` cloned at `vbar->mapFromGlobal
    // (event->globalPosition().toPoint())` once we hit a case
    // that cares.
    QApplication::sendEvent(vbar, event);
    // Mark accepted so the parent scroll area doesn't double-
    // process the wheel event.
    event->accept();
}

void OverviewRailWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    SyncBucketCountToHeight();
}

void OverviewRailWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Re-arm the model after a hide-toggle dropped its bucket
    // vector. Idempotent when the height matches — `SetBucketCount`
    // is a no-op on a matching count.
    //
    // Skip the call when the widget hasn't been sized yet (first
    // show, before the hosting `LogTableView::UpdateOverviewRail
    // Geometry` runs). Calling `SetBucketCount(0)` here would
    // fire a redundant no-op transition; the follow-up
    // `resizeEvent` immediately supersedes it with the real
    // height.
    if (height() > 0)
    {
        SyncBucketCountToHeight();
    }
}

void OverviewRailWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    // Style / palette / font change may shift the DPI-fluent
    // width and the wash colour. Force a fresh sizeHint so the
    // hosting `LogTableView` reserves the right margin.
    // `ScreenChangeInternal` is a private Qt enum today; we
    // rely on it as the most reliable cross-version proxy for a
    // DPR change (Qt 6.6+ ships public `DevicePixelRatioChange`,
    // but the project targets Qt 6.1+). Both events go through
    // this switch so the newer one is picked up for free once we
    // raise the floor. See LogTableView::changeEvent for the
    // matching comment.
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
    // The rail is a whole-file overview: rail top = first row,
    // rail bottom = last row, bars fill the entire widget
    // height. Following klogg / glogg / VS Code minimap, the
    // vertical position within the rail is a linear projection
    // of the file's row index -- *not* of the viewport's Y
    // range. That means the bar for row 0 sits at the top edge
    // of the rail widget (level with the header's top edge),
    // and the currently visible viewport is drawn as a movable
    // highlight box (`ComputeViewportIndicatorRect`) *inside*
    // the rail.
    //
    // Restricting the rail to the viewport-Y strip only was
    // tried; users read that as "the rail is broken -- half of
    // it is empty chrome". A whole-widget projection gives the
    // full file overview at a glance and reserves the "where am
    // I" answer to the highlight box.
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
    // `indexAt(bottom)` returns -1 when the bottom is past the
    // last row; use the row count in that case so the indicator
    // extends to the tail as the user scrolls into the last page.
    const int bottomIdxRow = mTableView->indexAt(QPoint(0, std::max(0, viewport.height() - 1))).row();
    int bottomRow = (bottomIdxRow >= 0) ? bottomIdxRow : (totalRows - 1);

    int visibleTop = (topRow >= 0) ? topRow : 0;
    bottomRow = std::max(bottomRow, visibleTop);
    // Clamp into `[0, totalRows)` so the indicator never runs
    // past the rail even under a transient row-count mismatch.
    visibleTop = std::clamp(visibleTop, 0, totalRows - 1);
    bottomRow = std::clamp(bottomRow, visibleTop, totalRows - 1);

    const int railHeight = rail.height();
    const long long yTop = (static_cast<long long>(visibleTop) * static_cast<long long>(railHeight)) /
                           static_cast<long long>(totalRows);
    const long long yBottom = (static_cast<long long>(bottomRow + 1) * static_cast<long long>(railHeight)) /
                              static_cast<long long>(totalRows);
    const int naturalHeight = static_cast<int>(yBottom - yTop);
    int indicatorHeight = std::max(naturalHeight, INDICATOR_MIN_HEIGHT_PX);
    // When the natural viewport-Y span is shorter than the
    // `INDICATOR_MIN_HEIGHT_PX` floor (the common case for tall
    // logs -- a 50 k-row session with 20 visible rows maps to
    // sub-pixel on the rail), expand around the *centre* of the
    // visible range instead of pinning to the top. Otherwise a
    // row selected in the middle of the viewport would appear
    // at the top edge of the min-height-inflated indicator on
    // the rail, misaligned with where the user sees the row in
    // the table (regression report: "the anchor is showed in
    // the middle of the tableview but the rail marks it at the
    // top of the current-view preview").
    const long long yCenter = (yTop + yBottom) / 2;
    int indicatorTop = rail.top() + static_cast<int>(yCenter) - (indicatorHeight / 2);
    // Clamp to the rail so the centering + min-height boost
    // don't push the indicator past either inset.
    if (indicatorTop + indicatorHeight > rail.bottom() + 1)
    {
        indicatorTop = rail.bottom() + 1 - indicatorHeight;
    }
    indicatorTop = std::max(indicatorTop, rail.top());

    return QRect(rail.left(), indicatorTop, rail.width(), indicatorHeight);
}

void OverviewRailWidget::SyncBucketCountToHeight()
{
    if (mModel == nullptr)
    {
        return;
    }
    const QRect rail = InteractiveRailRect();
    const int height = std::max(0, rail.height());
    // One bucket per pixel row keeps the rail's paint math
    // trivial (fillRect at each Y). SetBucketCount is a no-op
    // when the count matches.
    mModel->SetBucketCount(static_cast<std::size_t>(height));
}

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
