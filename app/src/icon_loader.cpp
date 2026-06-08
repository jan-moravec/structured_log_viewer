#include "icon_loader.hpp"

#include <QApplication>
#include <QColor>
#include <QPainter>
#include <QPalette>
#include <QPixmap>
#include <QSize>
#include <QStyle>
#include <QSvgRenderer>
#include <QToolBar>
#include <QWidget>
#include <QtGlobal>

namespace icon_loader
{

namespace
{

/// Floor for the rendered edge length. Below this Lucide glyphs
/// turn into a smudge regardless of the source SVG; flooring also
/// guards against `pixelMetric` returning 0 / -1 on headless or
/// minimal-styled platforms.
constexpr int MIN_ICON_PX = 12;

/// Fallback edge length when no anchor / no style is available. Matches
/// Qt's typical `PM_LargeIconSize` on Windows + macOS so an unanchored
/// load looks the same as the anchored one in practice.
constexpr int FALLBACK_ICON_PX = 20;

} // namespace

QPixmap MakeThemedPixmap(const QString &resourcePath, const QColor &tintColor, int sizePx, qreal devicePixelRatio)
{
    QSvgRenderer renderer(resourcePath);
    if (!renderer.isValid())
    {
        // `:/...` resources that don't exist return an invalid
        // renderer. Degrade gracefully -- the caller will paint a
        // text-only button.
        return {};
    }

    const int edge = qMax(sizePx, MIN_ICON_PX);
    const qreal dpr = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;

    QPixmap pix(QSize(edge, edge) * dpr);
    pix.setDevicePixelRatio(dpr);
    pix.fill(Qt::transparent);

    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    // First pass: paint the SVG with its native colour. The mask
    // step replaces the RGB but needs the alpha channel populated.
    renderer.render(&painter, QRectF(0, 0, edge, edge));
    // Second pass: replace every non-transparent pixel with `tintColor`,
    // keeping the alpha intact. `CompositionMode_SourceIn` is the
    // canonical "tint a monochrome glyph" recipe.
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(pix.rect(), tintColor);
    painter.end();

    return pix;
}

QIcon MakeThemedIcon(const QString &resourcePath, const QColor &tintColor, int sizePx, qreal devicePixelRatio)
{
    const QPixmap pix = MakeThemedPixmap(resourcePath, tintColor, sizePx, devicePixelRatio);
    if (pix.isNull())
    {
        return {};
    }
    return QIcon{pix};
}

QIcon MakeThemedIcon(const QString &resourcePath, const QWidget *anchor)
{
    QPalette palette = (anchor != nullptr) ? anchor->palette() : QApplication::palette();
    const QColor tint = palette.color(QPalette::Active, QPalette::WindowText);

    const qreal dpr = (anchor != nullptr) ? anchor->devicePixelRatioF() : qApp->devicePixelRatio();

    int sizePx = FALLBACK_ICON_PX;
    // Prefer the toolbar's actual `iconSize` over the style's
    // `PM_LargeIconSize`: a toolbar that pins itself to e.g. 20 px
    // would otherwise be handed a 32-48 px pixmap on platforms
    // whose style reports a larger value, forcing Qt to downsample
    // every paint. Same-size rasterisation also keeps stroke
    // widths consistent across themes.
    if (const auto *toolbar = qobject_cast<const QToolBar *>(anchor); toolbar != nullptr)
    {
        if (const QSize iconSize = toolbar->iconSize(); iconSize.width() > 0)
        {
            sizePx = iconSize.width();
        }
    }
    else if (const QStyle *style = (anchor != nullptr) ? anchor->style() : QApplication::style(); style != nullptr)
    {
        if (const int metric = style->pixelMetric(QStyle::PM_LargeIconSize, nullptr, anchor); metric > 0)
        {
            sizePx = metric;
        }
    }

    return MakeThemedIcon(resourcePath, tint, sizePx, dpr);
}

} // namespace icon_loader
