#pragma once

#include <QIcon>
#include <QPixmap>

class QString;
class QColor;
class QWidget;

/// Small helpers for loading vector icons (`.svg` resources) and
/// tinting them to follow the active palette. Built for the
/// primary toolbar's Lucide icon set but reusable from any widget
/// that wants a single asset to render correctly under both light
/// and dark themes.
///
/// Implementation strategy: render the SVG into a `QPixmap` at the
/// requested DPR, then mask the pixmap with the tint colour via
/// `QPainter::CompositionMode_SourceIn`. The mask preserves the
/// SVG's alpha channel (anti-aliased edges + line strokes) while
/// replacing the source RGB with the tint. The Lucide assets are
/// monochrome line drawings, so this keeps them visually faithful
/// without per-theme asset variants.
///
/// All resource paths must resolve through Qt's resource system
/// (the `:/icons/...` prefix from `resources/resources.qrc`).
/// Anything that fails to load is returned as an empty `QIcon` /
/// `QPixmap` rather than asserting -- a missing icon should
/// degrade to a text-only button, not crash the toolbar.
namespace icon_loader
{

/// Resolved parameters for one themed-icon render: the tint colour
/// pulled from the anchor's palette, the rasterisation edge length
/// (logical px) preferred by the anchor, and the anchor's device
/// pixel ratio. Returned by `ResolveAnchorIconParams` so callers
/// that need to mint multiple pixmaps for the same anchor (e.g.
/// Off + On states of a checkable action) resolve the policy once
/// and reuse it -- both keeps the two pixmaps pixel-aligned and
/// avoids per-call `pixelMetric` queries.
struct IconRenderParams
{
    QColor tint;
    int sizePx = 0;
    qreal devicePixelRatio = 1.0;
};

/// Resolve render parameters for @p anchor:
/// * Tint = `anchor->palette().color(QPalette::Active, QPalette::WindowText)`,
///   or `QApplication::palette()` when @p anchor is null.
/// * DPR  = `anchor->devicePixelRatioF()`, or the app-wide DPR.
/// * Size = `QToolBar::iconSize().width()` when @p anchor is a
///   toolbar (matches what the toolbar will actually display, so
///   Qt does not have to downsample), else
///   `style->pixelMetric(QStyle::PM_LargeIconSize, nullptr, anchor)`,
///   else an internal fallback. Same-size rasterisation also keeps
///   stroke widths consistent across themes.
[[nodiscard]] IconRenderParams ResolveAnchorIconParams(const QWidget *anchor);

/// Render @p resourcePath into a `QPixmap` masked in @p tintColor
/// at @p sizePx (logical edge length), scaled to the requested
/// device pixel ratio @p devicePixelRatio. The returned pixmap is
/// ready to feed `QIcon::addPixmap` for stateful icons (e.g. a
/// checkable button that needs different glyphs for On / Off).
///
/// @p sizePx is clamped to a sane minimum so a zero-or-negative
/// metric (which `QStyle::pixelMetric` can return on some headless
/// platforms) does not produce a degenerate `QPixmap`.
///
/// SVGs that use the CSS `currentColor` keyword (e.g. the Lucide
/// icon set) are pre-processed before rendering: Qt's
/// `QSvgRenderer` does not implement `currentColor`, and its
/// fallback for an unresolved reference is implementation-defined
/// (today opaque black; a future Qt could erase the glyph
/// entirely). Substituting an opaque sentinel keeps the alpha
/// channel populated so the `SourceIn` mask below has something
/// to colour.
[[nodiscard]] QPixmap MakeThemedPixmap(
    const QString &resourcePath, const QColor &tintColor, int sizePx, qreal devicePixelRatio
);

/// Convenience wrapper around `MakeThemedPixmap` for the common
/// single-state case.
[[nodiscard]] QIcon MakeThemedIcon(
    const QString &resourcePath, const QColor &tintColor, int sizePx, qreal devicePixelRatio
);

/// Convenience overload: resolves the tint / size / DPR via
/// `ResolveAnchorIconParams(anchor)`, then forwards to the
/// explicit-parameters overload. Passing `nullptr` falls back to
/// the application palette / DPR / style.
[[nodiscard]] QIcon MakeThemedIcon(const QString &resourcePath, const QWidget *anchor);

} // namespace icon_loader
