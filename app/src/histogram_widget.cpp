#include "histogram_widget.hpp"

#include "histogram_model.hpp"
#include "theme_control.hpp"

#include <loglib/histogram_bucket_index.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_value.hpp>

#include <QBrush>
#include <QColor>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QKeyEvent>
#include <QTimeZone>
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

namespace
{

/// Painted stack order, bottom to top. Level with the higher severity
/// paints on top so error / fatal spikes are visually dominant.
/// `Unknown` sits at the bottom so unstyled rows are visible but
/// don't hide meaningful segments.
constexpr std::array<loglib::LogLevel, 7> STACK_ORDER = {
    loglib::LogLevel::Unknown,
    loglib::LogLevel::Trace,
    loglib::LogLevel::Debug,
    loglib::LogLevel::Info,
    loglib::LogLevel::Warn,
    loglib::LogLevel::Error,
    loglib::LogLevel::Fatal,
};

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

QString FormatTimestampForZoom(qint64 epochMicros, loglib::HistogramBucketSize size)
{
    // Show only as much precision as the bucket width justifies:
    // - 1 s/10 s buckets -> HH:mm:ss
    // - 1 min/10 min      -> HH:mm
    // - 1 h               -> yyyy-MM-dd HH:00
    // - 1 d               -> yyyy-MM-dd
    const auto dt = QDateTime::fromMSecsSinceEpoch(epochMicros / 1000, QTimeZone::UTC);
    switch (size)
    {
    case loglib::HistogramBucketSize::OneSecond:
    case loglib::HistogramBucketSize::TenSeconds:
        return dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
    case loglib::HistogramBucketSize::OneMinute:
    case loglib::HistogramBucketSize::TenMinutes:
        return dt.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    case loglib::HistogramBucketSize::OneHour:
        return dt.toString(QStringLiteral("yyyy-MM-dd HH:00"));
    case loglib::HistogramBucketSize::OneDay:
        return dt.toString(QStringLiteral("yyyy-MM-dd"));
    }
    return dt.toString(Qt::ISODate);
}

qint64 EpochMicros(loglib::TimeStamp ts)
{
    return ts.time_since_epoch().count();
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
        connect(mModel, &HistogramModel::bucketsChanged, this, qOverload<>(&QWidget::update));
        connect(mModel, &HistogramModel::timeColumnAvailabilityChanged, this, [this](bool /*hasTime*/) {
            update();
        });
    }
    if (mTheme != nullptr)
    {
        connect(mTheme, &ThemeControl::themeChanged, this, qOverload<>(&QWidget::update));
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
    const QString bucketLabel = QString::fromLatin1(
        loglib::HistogramBucketSizeLabel(idx.BucketSize()).data(),
        static_cast<qsizetype>(loglib::HistogramBucketSizeLabel(idx.BucketSize()).size())
    );
    QString rangePart;
    if (!idx.Empty())
    {
        const auto first = EpochMicros(idx.BucketStart(0));
        const auto last = EpochMicros(idx.BucketEnd(idx.Buckets().size() - 1));
        rangePart = QStringLiteral("  %1 \u2013 %2")
                        .arg(FormatTimestampForZoom(first, idx.BucketSize()))
                        .arg(FormatTimestampForZoom(last, idx.BucketSize()));
    }
    const QLocale locale = QLocale::system();
    return QStringLiteral("bucket: %1  \u00b7  rows: %2%3")
        .arg(bucketLabel)
        .arg(locale.toString(static_cast<qulonglong>(idx.TotalRowCount())))
        .arg(rangePart);
}

void HistogramWidget::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, false);

    const QRect plotRect = PlotRect();

    // Subtitle line at the top.
    painter.setPen(palette().color(QPalette::WindowText));
    painter.drawText(
        QRect(PLOT_HORIZONTAL_PADDING, 0, width() - (2 * PLOT_HORIZONTAL_PADDING), SUBTITLE_HEIGHT),
        Qt::AlignVCenter | Qt::AlignLeft,
        FormatSubtitle()
    );

    // Empty-state placeholder: no model, no time column, or empty index.
    const bool empty =
        mModel == nullptr || !mModel->HasTimeColumn() || mModel->Index().Empty() || plotRect.height() <= 0;
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
        (void)event;
        return;
    }

    const auto &idx = mModel->Index();
    const auto buckets = idx.Buckets();
    const std::size_t nBuckets = buckets.size();

    // Compute the tallest bucket total so bars scale to the plot
    // rect. Use double to keep the per-segment math stable at
    // extreme aspect ratios.
    uint32_t maxTotal = 0;
    for (const auto &b : buckets)
    {
        maxTotal = std::max(maxTotal, b.Total());
    }
    if (maxTotal == 0)
    {
        (void)event;
        return;
    }

    const double plotWidth = plotRect.width();
    const double plotHeight = plotRect.height();
    // Aggregate consecutive buckets into single visual columns when
    // the bar width would drop below 1 px. `stride` is 1 when every
    // bucket gets its own pixel or more.
    const std::size_t stride = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(static_cast<double>(nBuckets) / std::max(1.0, plotWidth))));
    const std::size_t columnCount = (nBuckets + stride - 1) / stride;
    const double columnWidth = plotWidth / static_cast<double>(columnCount);

    for (std::size_t col = 0; col < columnCount; ++col)
    {
        const std::size_t start = col * stride;
        const std::size_t end = std::min(nBuckets, start + stride);
        loglib::LevelBucket merged{};
        for (std::size_t i = start; i < end; ++i)
        {
            for (std::size_t s = 0; s < merged.counts.size(); ++s)
            {
                merged.counts[s] += buckets[i].counts[s];
            }
        }
        const uint32_t total = merged.Total();
        if (total == 0)
        {
            continue;
        }
        const double columnX = plotRect.left() + (static_cast<double>(col) * columnWidth);
        const double columnPixelWidth = std::max(1.0, columnWidth - 0.5);
        const double totalHeight = plotHeight * (static_cast<double>(total) / static_cast<double>(maxTotal));
        double stackTop = plotRect.bottom() + 1.0 - totalHeight;
        for (loglib::LogLevel level : STACK_ORDER)
        {
            const uint32_t count = merged.counts[static_cast<std::size_t>(level)];
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

    // Draw the drag brush overlay on top.
    if (mDragging)
    {
        const int x1 = std::min(mDragStartX, mDragCurrentX);
        const int x2 = std::max(mDragStartX, mDragCurrentX);
        const QRect brushRect(x1, plotRect.top(), std::max(1, x2 - x1), plotRect.height());
        QColor brushColor = palette().color(QPalette::Highlight);
        brushColor.setAlpha(DRAG_BRUSH_ALPHA);
        painter.fillRect(brushRect, brushColor);
        painter.setPen(palette().color(QPalette::Highlight));
        painter.drawRect(brushRect.adjusted(0, 0, -1, -1));
    }
    (void)event;
}

std::optional<std::size_t> HistogramWidget::BucketAtX(int x) const
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
    const int clampedX = std::clamp(x, plotRect.left(), plotRect.right());
    const double norm = static_cast<double>(clampedX - plotRect.left()) / static_cast<double>(plotRect.width());
    const std::size_t nBuckets = mModel->Index().Buckets().size();
    auto bucketIdx = static_cast<std::size_t>(norm * static_cast<double>(nBuckets));
    if (bucketIdx >= nBuckets)
    {
        bucketIdx = nBuckets - 1;
    }
    return bucketIdx;
}

void HistogramWidget::UpdateHoverTooltip(int mouseX)
{
    if (mModel == nullptr)
    {
        QToolTip::hideText();
        return;
    }
    const auto bucketOpt = BucketAtX(mouseX);
    if (!bucketOpt.has_value())
    {
        QToolTip::hideText();
        mLastHoverBucket = -1;
        return;
    }
    const std::size_t bucketIdx = *bucketOpt;
    if (static_cast<int>(bucketIdx) == mLastHoverBucket)
    {
        return;
    }
    mLastHoverBucket = static_cast<int>(bucketIdx);
    const auto &idx = mModel->Index();
    const auto &bucket = idx.Buckets()[bucketIdx];
    const auto start = EpochMicros(idx.BucketStart(bucketIdx));
    const auto end = EpochMicros(idx.BucketEnd(bucketIdx));
    QString body = tr("%1 \u2013 %2\ntotal: %3")
                       .arg(FormatTimestampForZoom(start, idx.BucketSize()))
                       .arg(FormatTimestampForZoom(end, idx.BucketSize()))
                       .arg(bucket.Total());
    for (auto level : STACK_ORDER)
    {
        const uint32_t count = bucket.counts[static_cast<std::size_t>(level)];
        if (count == 0)
        {
            continue;
        }
        body.append(QStringLiteral("\n"));
        body.append(QString::fromLatin1(
            loglib::CanonicalLevelName(level).data(),
            static_cast<qsizetype>(loglib::CanonicalLevelName(level).size())
        ));
        body.append(QStringLiteral(": "));
        body.append(QString::number(count));
    }
    QToolTip::showText(mapToGlobal(QPoint(mouseX, PLOT_TOP_PADDING)), body, this);
}

void HistogramWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
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
    UpdateHoverTooltip(event->pos().x());
    if (mDragStartX >= 0 && (event->buttons() & Qt::LeftButton) != 0)
    {
        mDragCurrentX = event->pos().x();
        if (std::abs(mDragCurrentX - mDragStartX) > DRAG_THRESHOLD_PIXELS)
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
        // Single click: emit bucketClicked.
        if (const auto bucketOpt = BucketAtX(releaseX); bucketOpt.has_value())
        {
            emit bucketClicked(*bucketOpt);
        }
        event->accept();
        return;
    }

    // Drag release: convert brush endpoints to a time range.
    const auto startBucket = BucketAtX(std::min(startX, releaseX));
    const auto endBucket = BucketAtX(std::max(startX, releaseX));
    if (!startBucket.has_value() || !endBucket.has_value() || mModel == nullptr)
    {
        event->accept();
        return;
    }
    const auto &idx = mModel->Index();
    const auto fromUs = EpochMicros(idx.BucketStart(*startBucket));
    // BucketEnd is exclusive; subtract 1 us so the caller-side
    // inclusive comparator ((from <= ts && ts <= to)) doesn't spill
    // into the next bucket.
    const auto toUs = EpochMicros(idx.BucketEnd(*endBucket)) - 1;
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
                mModel->ApplyAutoBucketSize();
                emit bucketSizeChanged(static_cast<int>(mModel->Index().BucketSize()));
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
    emit bucketSizeChanged(current - 1);
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
    emit bucketSizeChanged(current + 1);
    update();
}

void HistogramWidget::CancelDrag()
{
    mDragging = false;
    mDragStartX = -1;
    mDragCurrentX = -1;
    update();
}
