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
/// Reused from the Follow-tail toolbar action. The Up variant is
/// the same pixmap rotated 180 degrees.
constexpr auto ARROW_SVG_PATH = ":/icons/arrow-down-to-line.svg";

/// Logical-pixel edge length for the arrow, sized to match the
/// surrounding text line height.
constexpr int ARROW_ICON_PX = 14;

/// Fade duration. Long enough to read as "appearing", short enough
/// not to linger over a fast typist / streaming session.
constexpr int FADE_DURATION_MS = 150;

/// Cap on the displayed count before switching to "999+". Keeps
/// the pill width bounded across locales.
constexpr int DISPLAYED_COUNT_CAP = 999;

/// HSL lightness split point for the hover-contrast branch: below
/// is treated as a dark theme (lighten on hover), above as light
/// (darken on hover).
constexpr qreal LIGHTNESS_DARK_THEME_THRESHOLD = 0.5;
} // namespace

JumpToTailPill::JumpToTailPill(QWidget *parent)
    : QToolButton(parent)
{
    setObjectName(QStringLiteral("jumpToTailPill"));
    setCursor(Qt::PointingHandCursor);
    // `Qt::TabFocus` keeps the pill reachable for keyboard /
    // screen-reader / switch-control users (`Space` / `Enter`
    // activate it via QAbstractButton).
    setFocusPolicy(Qt::TabFocus);
    // `TextBesideIcon` honours QSS padding consistently across
    // platforms; the platform default can strip the icon on some
    // Windows styles.
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setAutoRaise(false);
    // `accessibleDescription` carries the action; the running
    // count goes into `accessibleName`, which `RebuildText`
    // refreshes (Qt's AT bridge prefers `accessibleName` over the
    // visible `text`, so without the refresh the count never
    // reaches assistive tech).
    setAccessibleDescription(tr("Jump to the newest row and re-engage Follow newest if streaming."));

    mOpacity = new QGraphicsOpacityEffect(this);
    mOpacity->setOpacity(0.0);
    setGraphicsEffect(mOpacity);

    mFade = new QPropertyAnimation(mOpacity, "opacity", this);
    mFade->setDuration(FADE_DURATION_MS);
    mFade->setEasingCurve(QEasingCurve::OutQuad);
    connect(mFade, &QPropertyAnimation::finished, this, [this]() {
        // Drop visibility once fully transparent so the hidden
        // pill leaves the input + accessibility tree.
        if (mOpacity->opacity() <= 0.0)
        {
            QToolButton::setVisible(false);
        }
    });

    ApplyStyleSheet();
    RefreshIcon();
    RebuildText();

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
    // Emit after `RebuildText` so a listener-driven
    // `PositionTailPill` sees the new sizeHint. Needed in both
    // directions: growth, shrink, and crossing the "999+" cap.
    emit contentSizeChanged();

    if (mCount > 0)
    {
        // Must `show()` before fading: a hidden widget does not
        // paint its graphics effect, so animating opacity is a
        // no-op visually.
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
        // Re-resolve QSS / icon against the new palette on theme
        // switches. The `mApplyingPalette` guard breaks the
        // recursion that Qt 6 induces when `setStyleSheet`
        // synchronously re-emits `PaletteChange`; an external
        // theme switch arrives outside our `setStyleSheet`, so
        // the guard is clear and the refresh runs.
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
    // Leading glyph mirrors the icon direction so screen readers
    // and copy-paste of the accessible text still convey up/down.
    const QString arrow = (mDirection == ArrowDirection::Down) ? QStringLiteral("\u2193") : QStringLiteral("\u2191");

    // Below the cap we use `%n` for plural-aware translation; past
    // the cap the count is bounded so a plural-agnostic `%1+`
    // string is clearer for translators. The English "new lines"
    // also covers count == 1: a 150 ms fade between "1 new line"
    // and "2 new lines" would flicker. Locales with strict plural
    // rules can still override in the catalog.
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

    // Refresh the accessible name so AT users hear the running
    // count, not just the static action label.
    if (mCount > 0)
    {
        setAccessibleName(tr("Jump to newest row, %1 new lines available").arg(mCount));
    }
    else
    {
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
        // Missing asset: degrade to text-only (same policy as the
        // other icon-loader call sites).
        setIcon(QIcon());
        setIconSize(QSize(ARROW_ICON_PX, ARROW_ICON_PX));
        return;
    }

    if (mDirection == ArrowDirection::Up)
    {
        // Rotate the "down to line" source to "up from line" for
        // newest-first mode.
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
    // Hitting this branch means a future caller forgot the
    // `mApplyingPalette` gate; assert in debug builds and keep
    // the early-return as a release-mode stack-overflow firewall
    // (see header doc on `mApplyingPalette`).
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

    // A generous fixed radius reads as "pill" at every realistic
    // font size and avoids a sizeHint round-trip on every theme
    // change.
    constexpr int RADIUS_PX = 12;
    constexpr int PADDING_VERTICAL_PX = 6;
    constexpr int PADDING_HORIZONTAL_PX = 12;

    const QColor bg = palette().color(QPalette::Active, QPalette::Highlight);
    const QColor fg = palette().color(QPalette::Active, QPalette::HighlightedText);

    // `QColor::darker` on a dark-theme highlight would pull the
    // hover *into* the surrounding black, hiding the pill on
    // hover. Branch on perceived lightness so the adjustment
    // always increases contrast: lighten on dark themes, darken
    // on light ones.
    const auto adjustForContrast = [](const QColor &c, int factor) {
        return c.lightnessF() < LIGHTNESS_DARK_THEME_THRESHOLD ? c.lighter(factor) : c.darker(factor);
    };
    const QColor hoverBg = adjustForContrast(bg, 110);
    const QColor pressedBg = adjustForContrast(bg, 125);
    // Reuse `HighlightedText` for the focus ring so it tracks the
    // foreground glyph through theme changes.
    const QColor focusRing = fg;

    const QString sheet = QStringLiteral("QToolButton#jumpToTailPill {"
                                         "  background-color: %1;"
                                         "  color: %2;"
                                         "  border: none;"
                                         "  border-radius: %3px;"
                                         "  padding: %4px %5px;"
                                         "  font-weight: 600;"
                                         "}"
                                         "QToolButton#jumpToTailPill:hover { background-color: %6; }"
                                         "QToolButton#jumpToTailPill:pressed { background-color: %7; }"
                                         "QToolButton#jumpToTailPill:focus { outline: 2px solid %8; }")
                              .arg(bg.name(QColor::HexRgb))
                              .arg(fg.name(QColor::HexRgb))
                              .arg(RADIUS_PX)
                              .arg(PADDING_VERTICAL_PX)
                              .arg(PADDING_HORIZONTAL_PX)
                              .arg(hoverBg.name(QColor::HexRgb))
                              .arg(pressedBg.name(QColor::HexRgb))
                              .arg(focusRing.name(QColor::HexRgb));
    // Skip the no-op `setStyleSheet`: a synchronous re-resolve
    // would re-emit `PaletteChange` / `StyleChange` and bounce
    // through the recursion guard for no reason.
    if (sheet == styleSheet())
    {
        return;
    }
    setStyleSheet(sheet);
}

void JumpToTailPill::FadeTo(qreal endOpacity)
{
    // Retarget any in-flight animation so a fade-in interrupted
    // by a fade-out (or vice versa) stays smooth instead of
    // snapping to the previous endpoint.
    if (mFade->state() == QAbstractAnimation::Running)
    {
        mFade->stop();
    }
    mFade->setStartValue(mOpacity->opacity());
    mFade->setEndValue(endOpacity);
    mFade->start();
}
