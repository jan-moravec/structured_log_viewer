#pragma once

#include <QIcon>

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
/// Anything that fails to load is returned as an empty `QIcon`
/// rather than asserting -- a missing icon should degrade to a
/// text-only button, not crash the toolbar.
namespace icon_loader
{

/// Render @p resourcePath into a `QIcon` whose pixmap is masked
/// in @p tintColor at @p sizePx (logical edge length), scaled to
/// the requested device pixel ratio @p devicePixelRatio.
///
/// @p sizePx is clamped to a sane minimum so a zero-or-negative
/// metric (which `QStyle::pixelMetric` can return on some headless
/// platforms) does not produce a degenerate `QPixmap`.
[[nodiscard]] QIcon
MakeThemedIcon(const QString &resourcePath, const QColor &tintColor, int sizePx, qreal devicePixelRatio);

/// Convenience overload: pulls the tint colour from
/// `anchor->palette().color(QPalette::Active, QPalette::WindowText)`,
/// the DPR from `anchor->devicePixelRatioF()`, and the size from
/// `style->pixelMetric(QStyle::PM_LargeIconSize, nullptr, anchor)`.
///
/// Passing `nullptr` falls back to `QApplication::palette()`,
/// `qApp->devicePixelRatio()`, and `QApplication::style()`.
[[nodiscard]] QIcon MakeThemedIcon(const QString &resourcePath, const QWidget *anchor);

} // namespace icon_loader
