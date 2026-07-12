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

#include <algorithm>
#include <bitset>
#include <climits>
#include <cstddef>

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

/// Fractional width of the match tick band inside the rail.
/// The band paints centred; ~55 % width reads as a clear
/// vertical marker without dominating the level wash beside
/// it.
constexpr double MATCH_TICK_WIDTH_FRACTION = 0.55;

/// Alpha on the level underlay. Muted enough that anchor and
/// match ticks stay legible when painted over the same bucket.
constexpr int LEVEL_UNDERLAY_ALPHA = 110;

/// Alpha on the rail wash. Subtle — a hint of separation,
/// not a slab. Applied over `QPalette::Base` so a Dark theme
/// dims and a Light theme lifts by the same delta.
constexpr int WASH_ALPHA = 24;

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
/// hittable target at very small font sizes.
constexpr int RAIL_MIN_WIDTH_PX = 10;

/// Maximum rail width in device-independent px. Some styles
/// (accessibility themes on Windows, GTK "chunky-scrollbar"
/// setups) advertise very large `PM_ScrollBarExtent` values —
/// mirroring them 1:1 would cover a large fraction of the
/// viewport with a nearly-empty rail. The cap keeps the rail
/// at klogg-parity density; ticks + indicator remain crisp
/// above this width so raising it further wastes pixels.
constexpr int RAIL_MAX_WIDTH_PX = 22;

/// Extra width added to the DPI-fluent metric. Nudges the
/// rail slightly wider than the scrollbar so it doesn't merge
/// visually with a scrollbar rendered in the same theme.
constexpr int RAIL_WIDTH_PADDING = 2;

QColor ColorForLevelWithFallback(const ThemeControl *theme, loglib::LogLevel level, const QPalette &palette)
{
    if (theme != nullptr)
    {
        const QBrush brush = theme->BackgroundFor(level);
        if (brush.style() != Qt::NoBrush)
        {
            return brush.color();
        }
    }
    // Fallback: use palette Highlight for higher severities,
    // Mid for lower. Consistent with the "no-theme test fixture"
    // path used by `HistogramWidget`.
    switch (level)
    {
    case loglib::LogLevel::Fatal:
    case loglib::LogLevel::Error:
    case loglib::LogLevel::Warn:
        return palette.color(QPalette::Highlight);
    case loglib::LogLevel::Info:
    case loglib::LogLevel::Debug:
    case loglib::LogLevel::Trace:
        return palette.color(QPalette::Mid);
    case loglib::LogLevel::Unknown:
    default:
        return palette.color(QPalette::Midlight);
    }
}

QColor ColorForAnchorSlot(const ThemeControl *theme, std::uint8_t colorIndex, const QPalette &palette)
{
    if (theme != nullptr)
    {
        const QBrush brush = theme->AnchorBrushFor(colorIndex, static_cast<int>(QPalette::Highlight));
        if (brush.style() != Qt::NoBrush)
        {
            return brush.color();
        }
    }
    return palette.color(QPalette::Highlight);
}

} // namespace

OverviewRailWidget::OverviewRailWidget(
    OverviewRailModel *model, ThemeControl *theme, QAbstractItemView *tableView, QWidget *parent
)
    : QWidget(parent), mModel(model), mTheme(theme), mTableView(tableView)
{
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(false);
    setCursor(Qt::ArrowCursor);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    setAutoFillBackground(false);

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
    // DPI-fluent width: anchor to the platform scrollbar extent
    // so the rail scales with the same metric users already
    // recognise. Add a small padding so the rail doesn't look
    // like a duplicate scrollbar when the two sit next to each
    // other. Fall back to the font's `M` advance when the style
    // returns a degenerate 0 (offscreen QPA in tests). Clamp
    // into `[RAIL_MIN_WIDTH_PX, RAIL_MAX_WIDTH_PX]` so a themed
    // style with a giant scrollbar extent doesn't inflate the
    // rail past the point where extra pixels stop conveying
    // information.
    const QStyle *s = style();
    const int scrollbarExtent = (s != nullptr) ? s->pixelMetric(QStyle::PM_ScrollBarExtent, nullptr, this) : 0;
    const int fontExtent = fontMetrics().horizontalAdvance(QLatin1Char('M'));
    const int raw = std::max({scrollbarExtent, fontExtent, RAIL_MIN_WIDTH_PX}) + RAIL_WIDTH_PADDING;
    const int extent = std::min(raw, RAIL_MAX_WIDTH_PX);
    return {extent, 0};
}

QSize OverviewRailWidget::minimumSizeHint() const
{
    return {RAIL_MIN_WIDTH_PX, 0};
}

int OverviewRailWidget::RailWidthForTest() const
{
    return sizeHint().width();
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

void OverviewRailWidget::paintEvent(QPaintEvent * /*event*/)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);

    const QPalette pal = palette();
    const QRect widgetRect = rect();

    // Wash background: base tinted a hair toward Window so the
    // rail reads as an edge frame. Painted edge-to-edge; the
    // interactive rail rect insets from this by
    // `RAIL_VERTICAL_INSET` for tick / indicator geometry.
    QColor washColor = pal.color(QPalette::Window);
    washColor.setAlpha(WASH_ALPHA);
    painter.fillRect(widgetRect, pal.color(QPalette::Base));
    painter.fillRect(widgetRect, washColor);

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

    // Pass 1: dominant-level underlay per non-empty bucket.
    // One pixel row per bucket by construction (buckets sized to
    // rail height via `SetBucketCount`). Skip the expensive
    // level lookup when the bucket is empty.
    for (std::size_t i = 0; i < nBuckets; ++i)
    {
        const auto &bucket = buckets[i];
        if (bucket.levels.Total() == 0)
        {
            continue;
        }
        const int y = railTop + static_cast<int>((i * static_cast<std::size_t>(railHeight)) / nBuckets);
        // Height of at least 1 px so the row is always visible.
        // Rounding via next-bucket-start avoids gaps at the
        // seams while keeping the layout tight.
        const int nextY = (i + 1 < nBuckets)
                              ? railTop + static_cast<int>(((i + 1) * static_cast<std::size_t>(railHeight)) / nBuckets)
                              : (railTop + railHeight);
        const int height = std::max(1, nextY - y);

        const loglib::LogLevel dominant = mModel->DominantLevel(i);
        QColor color = ColorForLevelWithFallback(mTheme, dominant, pal);
        color.setAlpha(LEVEL_UNDERLAY_ALPHA);
        painter.fillRect(QRect(underlayLeft, y, underlayWidth, height), color);
    }

    // Pass 2: match ticks. Painted as a narrower band centred
    // on the rail so it reads as an accent stripe separate
    // from the level underlay.
    if (mModel->HasMatchTicks())
    {
        const int tickWidth = std::max(2, static_cast<int>(underlayWidth * MATCH_TICK_WIDTH_FRACTION));
        const int tickLeft = underlayLeft + ((underlayWidth - tickWidth) / 2);
        for (std::size_t i = 0; i < nBuckets; ++i)
        {
            if (buckets[i].matchCount == 0)
            {
                continue;
            }
            const int y = railTop + static_cast<int>((i * static_cast<std::size_t>(railHeight)) / nBuckets);
            const int nextY =
                (i + 1 < nBuckets)
                    ? railTop + static_cast<int>(((i + 1) * static_cast<std::size_t>(railHeight)) / nBuckets)
                    : (railTop + railHeight);
            const int height = std::max(1, nextY - y);
            painter.fillRect(QRect(tickLeft, y, tickWidth, height), highlightColor);
        }
    }

    // Pass 3: anchor ticks. One filled sub-band per set slot,
    // aligned to the *right* edge so anchors and matches sit
    // side-by-side and don't overlay each other.
    if (mModel->HasAnchorTicks())
    {
        const int anchorBandWidth = std::max(2, underlayWidth / 3);
        const int anchorBandLeft = underlayLeft + underlayWidth - anchorBandWidth;
        for (std::size_t i = 0; i < nBuckets; ++i)
        {
            const auto &mask = buckets[i].anchorSlots;
            if (mask.none())
            {
                continue;
            }
            const int y = railTop + static_cast<int>((i * static_cast<std::size_t>(railHeight)) / nBuckets);
            const int nextY =
                (i + 1 < nBuckets)
                    ? railTop + static_cast<int>(((i + 1) * static_cast<std::size_t>(railHeight)) / nBuckets)
                    : (railTop + railHeight);
            const int height = std::max(1, nextY - y);
            // Paint the lowest-index set slot: multiple anchor
            // colours in one bucket collapse to the first-set
            // (deterministic, and rare for a small rail).
            for (std::size_t s = 0; s < mask.size(); ++s)
            {
                if (mask.test(s))
                {
                    const QColor c = ColorForAnchorSlot(mTheme, static_cast<std::uint8_t>(s), pal);
                    painter.fillRect(QRect(anchorBandLeft, y, anchorBandWidth, height), c);
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
    SyncBucketCountToHeight();
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
    const int totalRows = mModel->ProxyRowCount();
    if (totalRows <= 0)
    {
        return {};
    }
    const QRect rail = InteractiveRailRect();
    if (rail.isEmpty())
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
    int indicatorTop = rail.top() + static_cast<int>(yTop);
    int indicatorHeight = static_cast<int>(yBottom - yTop);
    indicatorHeight = std::max(indicatorHeight, INDICATOR_MIN_HEIGHT_PX);
    // Clamp to the rail so the min-height boost doesn't push
    // the indicator past the bottom insets.
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
