#include "jump_to_tail_pill.hpp"

#include "icon_loader.hpp"

#include <QEasingCurve>
#include <QEvent>
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

/// HSL lightness threshold that splits "this background needs to
/// be darkened for hover contrast" from "this background needs
/// to be lightened". Mid-grey by definition; named so the
/// theme-branching helper in `ApplyStyleSheet` reads as a policy
/// rather than a magic number.
constexpr qreal LIGHTNESS_DARK_THEME_THRESHOLD = 0.5;
} // namespace

JumpToTailPill::JumpToTailPill(QWidget *parent)
    : QToolButton(parent)
{
    setObjectName(QStringLiteral("jumpToTailPill"));
    setCursor(Qt::PointingHandCursor);
    // `Qt::TabFocus` so keyboard-only users can land on the pill
    // and activate it with `Space` / `Enter` (`QAbstractButton`
    // wires those automatically). `Qt::NoFocus` would have
    // stranded screen-reader / switch-control users; the click
    // affordance is otherwise mouse-only.
    setFocusPolicy(Qt::TabFocus);
    // Icon + text laid out side by side so the arrow visually
    // leads the count. `ToolButtonTextBesideIcon` honours QSS
    // padding consistently across platforms; the
    // `ToolButtonFollowStyle` default leaves the layout up to the
    // platform theme and on some Windows styles strips the icon
    // entirely.
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setAutoRaise(false);
    // Accessible description carries the *action*; the accessible
    // name is rebuilt with the running count by `RebuildText` so
    // screen readers announce "Jump to newest row, 5 new lines"
    // and follow live updates. Without the per-count refresh,
    // assistive tech would see only "Jump to newest row" with no
    // running tally -- the visible label's count would never
    // reach the user.
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
    // Notify the host *after* `RebuildText` has updated the label
    // so a `PositionTailPill` triggered by this signal sees the
    // new sizeHint. Two-direction repositioning is critical for
    // counts that cross the "999+" cap (label width changes by
    // more than a digit's worth) and for shrinking counts that
    // would otherwise leave the pill stuck off-centre against the
    // viewport edge.
    emit contentSizeChanged();

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

    // Two branches:
    //
    //   * Past the display cap we render `"%1+ new lines"` -- a
    //     bounded literal whose `+` suffix is a math/notation
    //     glyph used identically across locales. The catalog
    //     entry is a separate string (one `%1`, plural-agnostic)
    //     so translators don't have to puzzle over what `%n`
    //     means against a capped-and-bounded number.
    //   * Below the cap we use `tr("%n new lines", ..., mCount)`.
    //     `%n` opts the source string into Qt's plural-aware
    //     translation: `lupdate` emits both singular and plural
    //     slots so locales with multiple plural forms (Polish,
    //     Russian, Arabic, ...) can pick the right one. The
    //     English source still reads "new lines" in the singular
    //     slot too -- intentional: the 150 ms fade-in would
    //     flicker between "1 new line" and "2 new lines" on
    //     every batch boundary and that visual jitter is more
    //     distracting than the technically-incorrect plural for
    //     count == 1. Locales that need a true singular form can
    //     override in the catalog without us touching the
    //     code.
    //
    // Arrow glyph is prepended outside `tr` so translators get
    // a clean message string and don't have to babysit the
    // direction marker.
    QString message;
    if (mCount > DISPLAYED_COUNT_CAP)
    {
        message = tr("%1+ new lines").arg(DISPLAYED_COUNT_CAP);
    }
    else
    {
        message = tr("%n new lines", "jump-to-tail pill running count", mCount);
    }
    setText(QStringLiteral("%1 %2").arg(arrow, message));

    // Live-update the accessible name so screen readers (NVDA,
    // VoiceOver, Narrator) announce the current count when focus
    // lands on the pill or when AT polls for state changes.
    // Without this refresh the static "Jump to newest row" set
    // in the ctor would mask every count update -- Qt's
    // accessibility bridge prefers `accessibleName` over the
    // visible `text` property, so the running count would never
    // reach AT users.
    if (mCount > 0)
    {
        // Two-arg `tr` so translators get the count as a plain
        // `%1` rather than wrestling with `%n` plurality on the
        // accessible name (the visible label already covers
        // plural-correctness; AT prosody is more forgiving).
        setAccessibleName(tr("Jump to newest row, %1 new lines available").arg(mCount));
    }
    else
    {
        // Idle accessible name for the brief windows when the
        // pill is technically still in the tree (fade-out in
        // flight) but conceptually empty.
        setAccessibleName(tr("Jump to newest row"));
    }
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
    // The only caller that could re-enter is `changeEvent`, and
    // it already gates on `!mApplyingPalette`, so reaching this
    // branch means a future caller forgot the gate. Assert in
    // debug builds (a noisy failure beats a silent skip masking
    // the wiring bug) but keep the early-return in release as a
    // stack-overflow firewall: the synchronous `PaletteChange`
    // that Qt 6 emits inside `setStyleSheet` would otherwise
    // recurse without bound -- see the header doc on
    // `mApplyingPalette`.
    Q_ASSERT_X(
        !mApplyingPalette,
        "JumpToTailPill::ApplyStyleSheet",
        "re-entered while the QSS apply is in flight; recursive caller must gate on mApplyingPalette"
    );
    if (mApplyingPalette)
    {
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

    // Hover / pressed must visibly contrast against `bg` on every
    // theme. `QColor::darker(N)` divides HSV `V` -- on a dark
    // theme that pulls the hover *into* the surrounding black,
    // making the pill almost vanish on hover. Branch on the
    // background's perceived lightness so the adjustment goes
    // toward more contrast in either direction:
    //   * light theme (typical `Highlight` ~ vivid blue,
    //     lightness > 0.5) -> darken on hover.
    //   * dark theme (typical `Highlight` ~ dim blue,
    //     lightness <= 0.5) -> lighten on hover.
    // Same factor (~10% / ~25%) on both sides so the feedback
    // intensity matches across themes.
    const auto adjustForContrast = [](const QColor &c, int factor) {
        return c.lightnessF() < LIGHTNESS_DARK_THEME_THRESHOLD ? c.lighter(factor) : c.darker(factor);
    };
    const QColor hoverBg = adjustForContrast(bg, 110);
    const QColor pressedBg = adjustForContrast(bg, 125);
    // Focus ring: a subtle outline that reads against both the
    // pill's `bg` and the surrounding viewport. Reusing
    // `HighlightedText` for the outline keeps the colour in lock-
    // step with the foreground glyph.
    const QColor focusRing = fg;

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
                              "QToolButton#jumpToTailPill:focus { outline: 2px solid %8; }"
    )
                              .arg(bg.name(QColor::HexRgb))
                              .arg(fg.name(QColor::HexRgb))
                              .arg(RADIUS_PX)
                              .arg(PADDING_VERTICAL_PX)
                              .arg(PADDING_HORIZONTAL_PX)
                              .arg(hoverBg.name(QColor::HexRgb))
                              .arg(pressedBg.name(QColor::HexRgb))
                              .arg(focusRing.name(QColor::HexRgb));
    // Idempotent fast-path: identical sheets are common when
    // `changeEvent(PaletteChange)` fires from a Qt internal that
    // didn't actually move any of our tracked roles (e.g. a font
    // metrics change). Skipping the `setStyleSheet` call avoids
    // a synchronous QSS re-resolve (which would re-emit
    // `PaletteChange` and `StyleChange` -- the original recursion
    // we have to guard against). The `mApplyingPalette` guard
    // remains the firewall; this branch keeps the common case
    // out of the firewall path entirely.
    if (sheet == styleSheet())
    {
        return;
    }
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
