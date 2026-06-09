#include "icon_loader.hpp"

#include <QApplication>
#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QIODevice>
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

/// Read @p resourcePath into a `QByteArray`, substituting the CSS
/// `currentColor` keyword with an opaque sentinel.
///
/// Why: Qt 6's `QSvgRenderer` does not implement `currentColor`.
/// Today its fallback paints unresolved references in opaque
/// black, which is exactly what the `SourceIn` mask in
/// `MakeThemedPixmap` needs. But the behaviour is undocumented
/// and a future Qt could just as plausibly render the reference
/// as transparent, which would erase every glyph that depends on
/// it (the entire Lucide set, since every Lucide icon uses
/// `stroke="currentColor"`). The sentinel guarantees we always
/// get opaque pixels regardless of how Qt resolves the reference
/// -- the actual colour does not matter because the mask
/// overwrites RGB.
///
/// Returns an empty array when the resource cannot be opened, so
/// the caller can degrade to a text-only button via the existing
/// `renderer.isValid()` check.
[[nodiscard]] QByteArray LoadSvgWithResolvedCurrentColor(const QString &resourcePath)
{
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }
    QByteArray data = file.readAll();
    // Case-sensitive replace; SVG / CSS treats the keyword as
    // case-insensitive but every authoring tool emits the camelCase
    // form (Lucide, Feather, Material, Tabler all do). Skipping the
    // case-insensitive walk keeps the per-call cost negligible.
    data.replace("currentColor", "#000000");
    return data;
}

} // namespace

IconRenderParams ResolveAnchorIconParams(const QWidget *anchor)
{
    IconRenderParams params;
    const QPalette palette = (anchor != nullptr) ? anchor->palette() : QApplication::palette();
    params.tint = palette.color(QPalette::Active, QPalette::WindowText);
    params.devicePixelRatio = (anchor != nullptr) ? anchor->devicePixelRatioF() : qApp->devicePixelRatio();

    params.sizePx = FALLBACK_ICON_PX;
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
            params.sizePx = iconSize.width();
        }
    }
    else if (const QStyle *style = (anchor != nullptr) ? anchor->style() : QApplication::style(); style != nullptr)
    {
        if (const int metric = style->pixelMetric(QStyle::PM_LargeIconSize, nullptr, anchor); metric > 0)
        {
            params.sizePx = metric;
        }
    }
    return params;
}

QPixmap MakeThemedPixmap(const QString &resourcePath, const QColor &tintColor, int sizePx, qreal devicePixelRatio)
{
    const QByteArray svgData = LoadSvgWithResolvedCurrentColor(resourcePath);
    if (svgData.isEmpty())
    {
        // Missing resource (typo, dropped from qrc, ...). Degrade
        // gracefully -- the caller will paint a text-only button.
        return {};
    }
    QSvgRenderer renderer(svgData);
    if (!renderer.isValid())
    {
        // Malformed SVG. Same degradation as above.
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
    // Logical extent of the pixmap. `QPixmap::rect()` reports
    // *device* pixels (`edge*dpr`), but the painter is set up in
    // logical coordinates because `pix` carries a DPR -- mixing
    // the two undershoots the fill when DPR < 1 and leaves a
    // fringe of the glyph in the opaque-black sentinel colour.
    // Same rect feeds both the SVG render and the tint mask so
    // they cover the exact same area.
    const QRectF logicalRect(0.0, 0.0, static_cast<qreal>(edge), static_cast<qreal>(edge));
    // First pass: paint the SVG with its native colour. The mask
    // step replaces the RGB but needs the alpha channel populated.
    renderer.render(&painter, logicalRect);
    // Second pass: replace every non-transparent pixel with `tintColor`,
    // keeping the alpha intact. `CompositionMode_SourceIn` is the
    // canonical "tint a monochrome glyph" recipe.
    painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
    painter.fillRect(logicalRect, tintColor);
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
    const IconRenderParams params = ResolveAnchorIconParams(anchor);
    return MakeThemedIcon(resourcePath, params.tint, params.sizePx, params.devicePixelRatio);
}

} // namespace icon_loader
