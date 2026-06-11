#include "level_cell_delegate.hpp"

#include "log_model.hpp"
#include "theme_control.hpp"

#include <loglib/log_level.hpp>

#include <QAbstractItemModel>
#include <QAbstractProxyModel>
#include <QApplication>
#include <QBrush>
#include <QIcon>
#include <QModelIndex>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPixmap>
#include <QRectF>
#include <QSize>
#include <QStyle>
#include <QStyleOptionViewItem>
#include <QStyledItemDelegate>
#include <QVariant>
#include <Qt>

#include <algorithm>
#include <optional>

namespace
{

/// Inset (logical px) the pill leaves on each side of the cell.
/// Mirrors what the upstream Lucide-styled "row badge" pattern
/// uses in their reference designs and what other log viewers
/// (Seq, lnav) ship with: enough surrounding chrome that the
/// pill reads as a separate shape from the row fill, but not so
/// much that a 24-px-tall row swallows the icon. The icon
/// rasterises at this much inset from the pill again, so the
/// glyph never touches the rounded edge.
constexpr int PILL_PADDING_PX = 4;

/// Inset between the pill edge and the icon glyph. Keeping this
/// independent of `PILL_PADDING_PX` lets us shrink the cell
/// without the icon also shrinking faster than the pill -- the
/// icon stays the visual focus, the pill is just a tint surface.
constexpr int ICON_INSET_INSIDE_PILL_PX = 2;

/// Pill corner radius. Half the cell height looks like a true
/// stadium; 6 px gives a "squircle" tag that matches the rest of
/// the IDE's chrome (toolbar buttons, dock-title chips).
constexpr qreal PILL_CORNER_RADIUS_PX = 6.0;

/// `2.0` keeps showing up as "min diameter from radius" /
/// "center offset = edge / 2"; pull it out so clang-tidy's
/// magic-number lint stays quiet and the intent reads clearly
/// at every call site.
constexpr qreal TWO = 2.0;

/// Walk a chain of proxy models to obtain the source-side index
/// rooted at the underlying `LogModel`. Returns an invalid
/// `QModelIndex` when the chain root is not a `LogModel` (e.g. a
/// test stub) -- callers fall through to the base class which
/// keeps text rendering.
QModelIndex MapToLogModelSource(const QModelIndex &index, const LogModel **outLogModel) noexcept
{
    if (outLogModel != nullptr)
    {
        *outLogModel = nullptr;
    }
    if (!index.isValid())
    {
        return {};
    }

    QModelIndex current = index;
    const QAbstractItemModel *model = current.model();
    while (model != nullptr)
    {
        if (const auto *logModel = qobject_cast<const LogModel *>(model))
        {
            if (outLogModel != nullptr)
            {
                *outLogModel = logModel;
            }
            return current;
        }
        const auto *proxy = qobject_cast<const QAbstractProxyModel *>(model);
        if (proxy == nullptr)
        {
            return {};
        }
        current = proxy->mapToSource(current);
        if (!current.isValid())
        {
            return {};
        }
        model = current.model();
    }
    return {};
}

} // namespace

LevelCellDelegate::LevelCellDelegate(ThemeControl *theme, QObject *parent)
    : QStyledItemDelegate(parent)
    , mTheme(theme)
{
}

const LogModel *LevelCellDelegate::ResolveLogModel(const QAbstractItemModel *model) const noexcept
{
    const QAbstractItemModel *current = model;
    while (current != nullptr)
    {
        if (const auto *logModel = qobject_cast<const LogModel *>(current))
        {
            return logModel;
        }
        const auto *proxy = qobject_cast<const QAbstractProxyModel *>(current);
        if (proxy == nullptr)
        {
            return nullptr;
        }
        current = proxy->sourceModel();
    }
    return nullptr;
}

void LevelCellDelegate::paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    // Self-gate: if icon mode is off (either the user toggled
    // it, the theme has no override, or this isn't a LogModel)
    // we fall straight through to the base delegate so text
    // rendering keeps working. This makes the install/detach
    // order in `MainWindow::ApplyLevelCellDelegate` an
    // optimisation rather than a correctness requirement.
    const LogModel *logModel = ResolveLogModel(index.model());
    if (logModel == nullptr || mTheme == nullptr || !logModel->IsLevelIconModeActive())
    {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    // Resolve the level for the row through the proxy chain so
    // we can pick the right pill brushes. Falls back to the base
    // delegate if anything along the chain is malformed.
    const LogModel *resolvedLogModel = nullptr;
    const QModelIndex sourceIndex = MapToLogModelSource(index, &resolvedLogModel);
    if (!sourceIndex.isValid() || resolvedLogModel == nullptr)
    {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    const auto level = resolvedLogModel->Table().GetLevelForRow(
        static_cast<size_t>(sourceIndex.row()), static_cast<size_t>(sourceIndex.column())
    );
    if (!level.has_value())
    {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    // Paint the cell's row chrome first: row tint + selection
    // overlay come from the QStyle so anchored rows / Level
    // background / selection still show through outside the pill.
    // We pass an empty-text option to suppress the base class's
    // text draw without losing the row fill.
    QStyleOptionViewItem fillOption = option;
    initStyleOption(&fillOption, index);
    fillOption.text = QString();
    fillOption.icon = QIcon();
    fillOption.features &= ~QStyleOptionViewItem::HasDecoration;
    const QWidget *widget = option.widget;
    const QStyle *style = (widget != nullptr) ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &fillOption, painter, widget);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);

    // Compute the pill rectangle: cell rect inset by
    // `PILL_PADDING_PX`. Width is clamped so the pill is never
    // narrower than its corner radius would imply (degenerate
    // shapes look like a square dot).
    const QRect cellRect = option.rect;
    QRectF pillRect = QRectF(cellRect).adjusted(
        PILL_PADDING_PX, PILL_PADDING_PX, -PILL_PADDING_PX, -PILL_PADDING_PX
    );
    if (pillRect.width() < PILL_CORNER_RADIUS_PX * TWO)
    {
        const qreal centerX = pillRect.center().x();
        pillRect.setLeft(centerX - PILL_CORNER_RADIUS_PX);
        pillRect.setRight(centerX + PILL_CORNER_RADIUS_PX);
    }
    if (pillRect.height() < PILL_CORNER_RADIUS_PX * TWO)
    {
        const qreal centerY = pillRect.center().y();
        pillRect.setTop(centerY - PILL_CORNER_RADIUS_PX);
        pillRect.setBottom(centerY + PILL_CORNER_RADIUS_PX);
    }

    const QBrush pillBackground = mTheme->PillBackgroundFor(*level);
    if (pillBackground.style() != Qt::NoBrush)
    {
        // Tip a clip path so anti-aliased pill edges stay inside
        // the inset rect even on fractional DPRs.
        QPainterPath path;
        path.addRoundedRect(pillRect, PILL_CORNER_RADIUS_PX, PILL_CORNER_RADIUS_PX);
        painter->fillPath(path, pillBackground);
    }

    // Icon: centred inside the pill with a further small inset
    // so the glyph never kisses the rounded edge. Renders at
    // `PM_SmallIconSize` (the standard Qt small-icon metric)
    // unless the pill is smaller, in which case we shrink the
    // glyph to fit. We let `QIcon::paint` handle DPR scaling -
    // the icon was rasterised at the right DPR when
    // `ThemeControl::BuildStyleCache` minted it.
    const QIcon icon = mTheme->IconFor(*level);
    if (!icon.isNull())
    {
        const int smallIcon = style->pixelMetric(QStyle::PM_SmallIconSize, &option, widget);
        const qreal availableWidth = pillRect.width() - (TWO * ICON_INSET_INSIDE_PILL_PX);
        const qreal availableHeight = pillRect.height() - (TWO * ICON_INSET_INSIDE_PILL_PX);
        const qreal iconEdge =
            std::max<qreal>(1.0, std::min<qreal>(smallIcon, std::min(availableWidth, availableHeight)));
        const QRectF iconRect = QRectF(
            pillRect.center().x() - (iconEdge / TWO), pillRect.center().y() - (iconEdge / TWO), iconEdge, iconEdge
        );
        // Always paint in `QIcon::Normal`: the cached icon was
        // minted as a single-mode `QIcon` (one `Normal` pixmap)
        // already tinted to `pillForeground`. Asking for
        // `QIcon::Selected` would route through
        // `QStyle::generatedIconPixmap`, which by default
        // overlays the highlight palette and clobbers the
        // themed tint. The row-level selection overlay is
        // already drawn behind the pill via the
        // `CE_ItemViewItem` fill above, so the cell still reads
        // as selected without re-tinting the glyph.
        icon.paint(painter, iconRect.toRect(), Qt::AlignCenter, QIcon::Normal);
    }

    painter->restore();
}

QSize LevelCellDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    // Default to the base delegate's hint so text-mode columns
    // (icon mode off) keep the historical row height. We only
    // tighten the *width* when icon mode is active.
    const QSize baseHint = QStyledItemDelegate::sizeHint(option, index);

    const LogModel *logModel = ResolveLogModel(index.model());
    if (logModel == nullptr || !logModel->IsLevelIconModeActive())
    {
        return baseHint;
    }

    const QWidget *widget = option.widget;
    const QStyle *style = (widget != nullptr) ? widget->style() : QApplication::style();
    const int smallIcon = style->pixelMetric(QStyle::PM_SmallIconSize, &option, widget);
    const int headerMark = style->pixelMetric(QStyle::PM_HeaderMarkSize, &option, widget);

    // Width budget: pill padding + icon-inset + icon + icon-inset
    // + pill padding, plus the header sort-indicator size so the
    // chevron stays visible when the user clicks the column.
    const int iconWidth = smallIcon + (2 * ICON_INSET_INSIDE_PILL_PX) + (2 * PILL_PADDING_PX);
    const int minWidth = iconWidth + std::max(0, headerMark);
    const int height = std::max(baseHint.height(), smallIcon + (2 * PILL_PADDING_PX));
    return QSize(std::max(baseHint.width(), minWidth), height);
}
