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
///
/// @p outLogModel receives the resolved `LogModel*` (or nullptr
/// when the chain root isn't a `LogModel`). Single-walk variant:
/// folds the model-resolve and the index-translate the paint
/// path used to do as two separate chain walks into one.
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
    // `sizeHint` doesn't need a translated index, only the model
    // identity; reusing `MapToLogModelSource` with an invalid index
    // would early-out before the qobject_cast chain so we keep a
    // dedicated index-free walk here.
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
    // Resolve the source-side `LogModel` and the source-side index
    // in a single proxy-chain walk: the gate check (`IsLevelIconModeActive`)
    // and the level lookup (`GetLevelForRow`) both need them, so
    // a separate `ResolveLogModel` call here would walk the chain
    // twice on the paint hot path.
    //
    // Self-gate: if anything is missing (no LogModel, no theme,
    // icon mode toggled off, or the proxy chain is malformed) we
    // fall straight through to the base delegate so text rendering
    // keeps working. This makes the install/detach order in
    // `MainWindow::ApplyLevelCellDelegate` an optimisation rather
    // than a correctness requirement.
    const LogModel *logModel = nullptr;
    const QModelIndex sourceIndex = MapToLogModelSource(index, &logModel);
    if (logModel == nullptr || mTheme == nullptr || !sourceIndex.isValid() || !logModel->IsLevelIconModeActive())
    {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    // `GetDisplayLevelForRow` (not `GetLevelForRow`) so an unmapped
    // raw value surfaces as `LogLevel::Unknown` -- the delegate
    // then paints the theme's generic "unknown" glyph rather than
    // falling through to text rendering. nullopt still means "no
    // value at all" (truly blank cell).
    const auto level = logModel->Table().GetDisplayLevelForRow(
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
    //
    // `initStyleOption` is required even though the view passed
    // us `option`: the view leaves the model-derived fields
    // (BackgroundRole / ForegroundRole / FontRole, decoration,
    // text) unpopulated, expecting each delegate to finalise them
    // for its index. Without this, the cell fill below would
    // ignore the theme's per-level background brush.
    QStyleOptionViewItem fillOption = option;
    initStyleOption(&fillOption, index);
    fillOption.text = QString();
    fillOption.icon = QIcon();
    // Also clear `HasDisplay`: with an empty `text` the QStyle still
    // reserves a text sub-rect (focus rect layout, sub-element
    // calculations) when the feature flag is set. Pairing the flag
    // clear with the field clear matches the "no text, no icon"
    // intent and keeps focus/selection geometry consistent with the
    // pill-only render.
    fillOption.features &= ~(QStyleOptionViewItem::HasDecoration | QStyleOptionViewItem::HasDisplay);
    const QWidget *widget = option.widget;
    const QStyle *style = (widget != nullptr) ? widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem, &fillOption, painter, widget);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    // Hard-clip to the cell so a degenerate column (user dragged
    // the level column narrower than the pill's minimum diameter)
    // can't bleed the pill / icon into the neighbour column.
    // `sizeHint` already pushes back on shrinking below the
    // icon-only width, but Qt honours manual section resizes
    // beyond the hint.
    painter->setClipRect(option.rect);

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
        // Use a clip path so anti-aliased pill edges stay inside
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
    // (icon mode off) keep the historical row height + width.
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
    const int width = iconWidth + std::max(0, headerMark);
    // Height: keep the base hint's row height (e.g. text font
    // ascent + leading from the QStyle) so icon rows align with
    // text rows visually; only force a floor when the icon would
    // overflow a too-small base hint.
    const int height = std::max(baseHint.height(), smallIcon + (2 * PILL_PADDING_PX));
    // In icon mode the *displayed* content is just the pill+icon;
    // returning the base hint's text width (e.g. for "WARNING")
    // would keep the column visually as wide as the longest
    // suppressed string. Returning the narrow icon width lets
    // `resizeColumnsToContents()` actually shrink the column to
    // the icon footprint, which is the headline visual change of
    // icon mode.
    return QSize(width, height);
}
