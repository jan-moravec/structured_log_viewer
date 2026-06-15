#include "jump_to_tail_pill.hpp"

#include "icon_loader.hpp"

#include <QEasingCurve>
#include <QEvent>
#include <QFontMetrics>
#include <QGraphicsOpacityEffect>
#include <QIcon>
#include <QLatin1String>
#include <QPalette>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QScopeGuard>
#include <QSize>
#include <QString>
#include <QStringLiteral>
#include <QTransform>

namespace
{
/// SVG asset reused from the `actionFollowTail` toolbar icon. A
/// single asset works for both directions: the `Up` variant is the
/// same pixmap rotated 180 degrees via `QTransform`.
constexpr auto ARROW_SVG_PATH = ":/icons/arrow-down-to-line.svg";

/// Logical-pixel edge length for the arrow glyph. The pill's row
/// height is driven by the text font metrics; this size matches a
/// typical body line height so the glyph reads as part of the
/// text run rather than as an oversized adornment.
constexpr int ARROW_ICON_PX = 14;

/// Fade duration; matches the Find bar's match-count debounce
/// envelope so two animation timescales don't visibly compete.
/// Long enough to read as "appearing", short enough that a fast
/// typist / streaming session doesn't see lingering animation.
constexpr int FADE_DURATION_MS = 150;

/// Cap on the literal value rendered in the pill before we switch
/// to "999+". The pill is informational; the user only needs to
/// know "a lot". Choosing a 3-digit cap also keeps the pill width
/// bounded across locales (grouped digits, RTL, etc.) so the
/// auto-positioning helper in the host view doesn't have to
/// repaint a wider pill mid-stream.
constexpr int DISPLAYED_COUNT_CAP = 999;
} // namespace

JumpToTailPill::JumpToTailPill(QWidget *parent)
    : QToolButton(parent)
{
    setObjectName(QStringLiteral("jumpToTailPill"));
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::NoFocus);
    // Icon + text laid out side by side so the arrow visually
    // leads the count. `ToolButtonTextBesideIcon` honours QSS
    // padding consistently across platforms; the
    // `ToolButtonFollowStyle` default leaves the layout up to the
    // platform theme and on some Windows styles strips the icon
    // entirely.
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setAutoRaise(false);
    // Accessible name reads to screen readers without the running
    // count, which the live text already carries; the description
    // adds the action so the affordance reads as a verb.
    setAccessibleName(tr("Jump to newest row"));
    setAccessibleDescription(tr("Jump to the newest row and re-engage Follow newest if streaming."));

    // Opacity effect on the widget itself; `setVisible(false)` at
    // the end of the fade-out so the hidden pill stops absorbing
    // hover events and accessibility tree walks.
    mOpacity = new QGraphicsOpacityEffect(this);
    mOpacity->setOpacity(0.0);
    setGraphicsEffect(mOpacity);

    mFade = new QPropertyAnimation(mOpacity, "opacity", this);
    mFade->setDuration(FADE_DURATION_MS);
    mFade->setEasingCurve(QEasingCurve::OutQuad);
    connect(mFade, &QPropertyAnimation::finished, this, [this]() {
        // Once fully transparent, drop visibility so the widget is
        // out of the input + accessibility tree. Re-shown by
        // `FadeTo(1.0)` before the next animation starts.
        if (mOpacity->opacity() <= 0.0)
        {
            QToolButton::setVisible(false);
        }
    });

    ApplyStyleSheet();
    RefreshIcon();
    RebuildText();

    // Pill is hidden + transparent until `SetCount(>0)` arms it.
    QToolButton::setVisible(false);
}

void JumpToTailPill::SetCount(int count)
{
    const int sanitised = count < 0 ? 0 : count;
    if (sanitised == mCount)
    {
        return;
    }
    mCount = sanitised;
    RebuildText();

    if (mCount > 0)
    {
        // Must `show()` before the fade starts: a widget with
        // `visible == false` does not paint its graphics effect,
        // so animating opacity while hidden is a no-op visually.
        if (!isVisible())
        {
            QToolButton::setVisible(true);
        }
        FadeTo(1.0);
    }
    else
    {
        FadeTo(0.0);
    }
}

void JumpToTailPill::SetArrowDirection(ArrowDirection direction)
{
    if (direction == mDirection)
    {
        return;
    }
    mDirection = direction;
    RefreshIcon();
    RebuildText();
}

void JumpToTailPill::changeEvent(QEvent *event)
{
    QToolButton::changeEvent(event);
    if (event == nullptr)
    {
        return;
    }
    switch (event->type())
    {
    case QEvent::PaletteChange:
    case QEvent::ApplicationPaletteChange:
        // Theme switches reach us via palette swaps (Light <->
        // Dark, custom themes). The QSS literal hex strings are
        // resolved against the *previous* palette, so re-apply
        // and re-tint the glyph.
        //
        // The `mApplyingPalette` guard breaks the implicit
        // recursion: `setStyleSheet` synchronously emits
        // `PaletteChange` / `StyleChange` on the widget on Qt 6
        // (the QSS resolver re-binds palette roles), and routing
        // those back into `ApplyStyleSheet` recurses forever
        // (stack overflow). A genuine external theme switch
        // arrives outside our `setStyleSheet` call, so the
        // guard is clear and the refresh runs.
        if (!mApplyingPalette)
        {
            ApplyStyleSheet();
            RefreshIcon();
        }
        break;
    default:
        break;
    }
}

void JumpToTailPill::RebuildText()
{
    // Leading glyph mirrors the icon direction so screen-readers
    // and copy-paste of the accessible text still convey "down"
    // / "up"; the icon is purely visual.
    const QString arrow =
        (mDirection == ArrowDirection::Down) ? QStringLiteral("\u2193") : QStringLiteral("\u2191");

    const int shown = mCount > DISPLAYED_COUNT_CAP ? DISPLAYED_COUNT_CAP : mCount;
    const QString countStr =
        mCount > DISPLAYED_COUNT_CAP ? tr("%1+").arg(shown) : QString::number(shown);

    // Always use the plural form. The pill is only visible for
    // counts >= 1; a literal "1 new line" would technically be
    // more correct but the singular flashes for one frame on every
    // batch boundary and reads as noise. The translator can still
    // override via `%Ln` if a target language needs different
    // pluralisation.
    const QString text = tr("%1 %2 new lines").arg(arrow, countStr);
    setText(text);
}

void JumpToTailPill::RefreshIcon()
{
    const QColor tint = palette().color(QPalette::Active, QPalette::HighlightedText);
    const qreal dpr = devicePixelRatioF() > 0.0 ? devicePixelRatioF() : 1.0;

    QPixmap pix = icon_loader::MakeThemedPixmap(QLatin1String(ARROW_SVG_PATH), tint, ARROW_ICON_PX, dpr);
    if (pix.isNull())
    {
        // Missing asset -- fall through to text-only. Same
        // degradation policy as the rest of the icon-loader call
        // sites (toolbar, headers).
        setIcon(QIcon());
        setIconSize(QSize(ARROW_ICON_PX, ARROW_ICON_PX));
        return;
    }

    if (mDirection == ArrowDirection::Up)
    {
        // Vertical flip: the source asset is "down to line"; the
        // 180-degree rotation produces an "up from line" glyph
        // that reads correctly when the tail is at the top of the
        // viewport (newest-first mode).
        const QTransform xform = QTransform().rotate(180);
        pix = pix.transformed(xform, Qt::SmoothTransformation);
    }

    QIcon icon;
    icon.addPixmap(pix);
    setIcon(icon);
    setIconSize(QSize(ARROW_ICON_PX, ARROW_ICON_PX));
}

void JumpToTailPill::ApplyStyleSheet()
{
    if (mApplyingPalette)
    {
        // Defensive: caller already holds the guard. Skip rather
        // than thrash the QSS twice.
        return;
    }
    mApplyingPalette = true;
    const auto guardRelease = qScopeGuard([this]() { mApplyingPalette = false; });

    // Pill height is driven by the QSS padding plus the icon /
    // font metric; computing `border-radius` from a measured
    // height would require a sizeHint round-trip after every
    // theme change. A generous fixed radius reads as "pill" at
    // every realistic font size and still rounds the corners
    // correctly even when the widget is shorter than 2*r.
    constexpr int RADIUS_PX = 12;
    constexpr int PADDING_VERTICAL_PX = 6;
    constexpr int PADDING_HORIZONTAL_PX = 12;

    // Resolve palette roles once so a theme switch picks up the
    // new colour names; the QSS itself uses literal hex (Qt does
    // not resolve `palette(...)` references on every role).
    const QColor bg = palette().color(QPalette::Active, QPalette::Highlight);
    const QColor fg = palette().color(QPalette::Active, QPalette::HighlightedText);
    // Slightly darker hover; `QColor::darker(110)` keeps the hue
    // and pushes the lightness ~10% down -- works on light AND
    // dark themes without bespoke per-mode tints.
    const QColor hoverBg = bg.darker(110);
    const QColor pressedBg = bg.darker(125);

    const QString sheet = QStringLiteral(
                              "QToolButton#jumpToTailPill {"
                              "  background-color: %1;"
                              "  color: %2;"
                              "  border: none;"
                              "  border-radius: %3px;"
                              "  padding: %4px %5px;"
                              "  font-weight: 600;"
                              "}"
                              "QToolButton#jumpToTailPill:hover { background-color: %6; }"
                              "QToolButton#jumpToTailPill:pressed { background-color: %7; }"
    )
                              .arg(bg.name(QColor::HexRgb))
                              .arg(fg.name(QColor::HexRgb))
                              .arg(RADIUS_PX)
                              .arg(PADDING_VERTICAL_PX)
                              .arg(PADDING_HORIZONTAL_PX)
                              .arg(hoverBg.name(QColor::HexRgb))
                              .arg(pressedBg.name(QColor::HexRgb));
    setStyleSheet(sheet);
}

void JumpToTailPill::FadeTo(qreal endOpacity)
{
    // Retarget any in-flight animation rather than chaining. Qt
    // restarts the QPropertyAnimation from the *current* opacity,
    // so a fade-in interrupted by a fade-out (or vice versa) is
    // smooth instead of jumping to the prior endpoint.
    if (mFade->state() == QAbstractAnimation::Running)
    {
        mFade->stop();
    }
    mFade->setStartValue(mOpacity->opacity());
    mFade->setEndValue(endOpacity);
    mFade->start();
}
