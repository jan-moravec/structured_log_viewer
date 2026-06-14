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

/// Inset between cell edge and pill edge.
constexpr int PILL_PADDING_PX = 4;

/// Inset between pill edge and icon glyph. Kept independent of
/// `PILL_PADDING_PX` so shrinking the cell doesn't squeeze the
/// icon faster than the pill.
constexpr int ICON_INSET_INSIDE_PILL_PX = 2;

/// Pill corner radius (squircle, matches the rest of the chrome).
constexpr qreal PILL_CORNER_RADIUS_PX = 6.0;

/// Named so clang-tidy's magic-number lint stays quiet on the
/// recurring "min diameter / centre offset" math below.
constexpr qreal TWO = 2.0;

/// Walk a proxy chain to the source `LogModel` and translate the
/// index in one pass. Returns an invalid index (and sets
/// `*outLogModel` to nullptr) when the chain root isn't a
/// `LogModel`.
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
    : QStyledItemDelegate(parent), mTheme(theme)
{
}

const LogModel *LevelCellDelegate::ResolveLogModel(const QAbstractItemModel *model) const noexcept
{
    // Index-free variant for `sizeHint`, which only needs the
    // model identity (not a translated index).
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
    // Resolve source model + source index in a single proxy-chain
    // walk; both the gate check and the level lookup need them.
    // Self-gate: anything missing -> fall through to the base
    // delegate, so install/detach order is just an optimisation.
    const LogModel *logModel = nullptr;
    const QModelIndex sourceIndex = MapToLogModelSource(index, &logModel);
    if (logModel == nullptr || mTheme == nullptr || !sourceIndex.isValid() || !logModel->IsLevelIconModeActive())
    {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    // `GetDisplayLevelForRow` so an unmapped value surfaces as
    // `Unknown` (generic glyph). nullopt = truly blank cell.
    const auto level = logModel->Table().GetDisplayLevelForRow(
        static_cast<size_t>(sourceIndex.row()), static_cast<size_t>(sourceIndex.column())
    );
    if (!level.has_value())
    {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    // Row chrome (tint + selection overlay) comes from the QStyle
    // so anchored rows / Level background / selection still show
    // outside the pill. Suppress the base's text + decoration by
    // clearing the fields *and* the feature flags so no text
    // sub-rect is reserved. `initStyleOption` populates the
    // model-derived fields (background, font, ...) which the view
    // leaves blank for delegates to finalise.
    QStyleOptionViewItem fillOption = option;
    initStyleOption(&fillOption, index);
    fillOption.text = QString();
    fillOption.icon = QIcon();
    fillOption.features &= ~(QStyleOptionViewItem::HasDecoration | QStyleOptionViewItem::HasDisplay);
    const QWidget *widget = option.widget;
    const QStyle *style = (widget != nullptr) ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &fillOption, painter, widget);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    // Hard-clip so a user-shrunk column can't bleed into its
    // neighbour. `sizeHint` only resists below the icon-only
    // width; manual resizes can still go narrower.
    painter->setClipRect(option.rect);

    // Pill rect: cell inset by `PILL_PADDING_PX`, clamped to at
    // least one diameter so degenerate columns don't draw a dot.
    const QRect cellRect = option.rect;
    QRectF pillRect = QRectF(cellRect).adjusted(PILL_PADDING_PX, PILL_PADDING_PX, -PILL_PADDING_PX, -PILL_PADDING_PX);
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
        // Clip path so anti-aliased edges stay inside the inset on
        // fractional DPRs.
        QPainterPath path;
        path.addRoundedRect(pillRect, PILL_CORNER_RADIUS_PX, PILL_CORNER_RADIUS_PX);
        painter->fillPath(path, pillBackground);
    }

    // Icon centred in the pill, sized to `PM_SmallIconSize` or
    // shrunk to fit when the pill is smaller. The cached icon was
    // already rasterised at the right DPR by `BuildStyleCache`.
    const QIcon icon = mTheme->IconFor(*level);
    if (!icon.isNull())
    {
        const int smallIcon = style->pixelMetric(QStyle::PM_SmallIconSize, &option, widget);
        const qreal availableWidth = pillRect.width() - (TWO * ICON_INSET_INSIDE_PILL_PX);
        const qreal availableHeight = pillRect.height() - (TWO * ICON_INSET_INSIDE_PILL_PX);
        const qreal iconEdge = std::max<qreal>(
            1.0, std::min<qreal>({static_cast<qreal>(smallIcon), availableWidth, availableHeight})
        );
        const QRectF iconRect = QRectF(
            pillRect.center().x() - (iconEdge / TWO), pillRect.center().y() - (iconEdge / TWO), iconEdge, iconEdge
        );
        // Force `QIcon::Normal`: the cached pixmap is already
        // tinted to `pillForeground`. `QIcon::Selected` would
        // route through `generatedIconPixmap` and overlay the
        // highlight palette, clobbering the tint. The selection
        // overlay was already drawn behind the pill above.
        icon.paint(painter, iconRect.toRect(), Qt::AlignCenter, QIcon::Normal);
    }

    painter->restore();
}

QSize LevelCellDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    // Fall back to the base hint when icon mode is off so text
    // columns keep the historical row height + width.
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

    // Width = pill (icon + 2 insets + 2 paddings) + room for the
    // header sort chevron. Height keeps the row height of text
    // rows for visual alignment; the icon footprint is the floor.
    // Returning a narrow width lets `resizeColumnsToContents()`
    // shrink the column to the icon -- the headline visual change
    // of icon mode.
    const int iconWidth = smallIcon + (2 * ICON_INSET_INSIDE_PILL_PX) + (2 * PILL_PADDING_PX);
    const int width = iconWidth + std::max(0, headerMark);
    const int height = std::max(baseHint.height(), smallIcon + (2 * PILL_PADDING_PX));
    return {width, height};
}
