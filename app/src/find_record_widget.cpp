#include "find_record_widget.hpp"

#include <QAction>
#include <QApplication>
#include <QColor>
#include <QDebug>
#include <QDockWidget>
#include <QEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPointF>
#include <QShortcut>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

namespace
{
/// Trailing-edge debounce after a keystroke / model bump before we
/// recount matches. Coalesces fast typing into one scan.
constexpr int MATCH_COUNT_DEBOUNCE_MS = 120;

/// Max age the debounced match-count emit can be deferred. The
/// trailing timer restarts on every bump and would never fire under
/// sustained activity (streaming, held keys), so this caps the lag.
constexpr int MATCH_COUNT_MAX_AGE_MS = 750;

/// Visual minimum for the search field.
constexpr int EDIT_MIN_WIDTH = 220;

/// Widget-level minimum so a floating `FindDock` can't be dragged
/// narrow enough to truncate the toggles or count label.
constexpr int WIDGET_MIN_WIDTH = 360;

constexpr int LAYOUT_OUTER_H_MARGIN = 6;
constexpr int LAYOUT_OUTER_V_MARGIN = 4;
constexpr int LAYOUT_SPACING = 4;

/// Reserved width for the count label so digits don't shift the arrow
/// buttons around as totals grow.
constexpr int MATCH_LABEL_MIN_WIDTH = 80;

/// Edge length for the arrow icons when `PM_SmallIconSize` is unavailable.
constexpr int ARROW_ICON_FALLBACK_PX = 14;

/// Triangle inset / centre as fractions of the icon edge. Tuned by
/// eye to match the visual weight of the `.*` / `*?` toggle glyphs.
constexpr qreal ARROW_INSET_RATIO = 0.18;
constexpr qreal ARROW_CENTRE_RATIO = 0.5;

/// Paint a chevron arrow icon in @p color (so it tracks the active
/// palette / theme). `pointingUp` selects prev (apex up) vs next
/// (apex down). @p devicePixelRatio sizes the backing pixmap so the
/// glyph stays sharp on HiDPI.
[[nodiscard]] QIcon MakeArrowIcon(int sizePx, bool pointingUp, const QColor &color, qreal devicePixelRatio)
{
    const qreal dpr = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;
    QPixmap pix(QSize(sizePx, sizePx) * dpr);
    pix.setDevicePixelRatio(dpr);
    pix.fill(Qt::transparent);
    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const qreal inset = sizePx * ARROW_INSET_RATIO;
    const qreal apex = pointingUp ? inset : sizePx - inset;
    const qreal base = pointingUp ? sizePx - inset : inset;
    QPainterPath triangle;
    triangle.moveTo(QPointF(sizePx * ARROW_CENTRE_RATIO, apex));
    triangle.lineTo(QPointF(inset, base));
    triangle.lineTo(QPointF(sizePx - inset, base));
    triangle.closeSubpath();
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPath(triangle);
    painter.end();
    return QIcon{pix};
}

[[nodiscard]] int ArrowIconPixels(const QWidget *widget)
{
    if (const QStyle *style = (widget != nullptr) ? widget->style() : QApplication::style(); style != nullptr)
    {
        const int metric = style->pixelMetric(QStyle::PM_SmallIconSize, nullptr, widget);
        if (metric > 0)
        {
            return metric;
        }
    }
    return ARROW_ICON_FALLBACK_PX;
}
} // namespace

FindRecordWidget::FindRecordWidget(QWidget *parent)
    : QWidget{parent}
{
    auto *hLayout = new QHBoxLayout(this);
    hLayout->setContentsMargins(
        LAYOUT_OUTER_H_MARGIN, LAYOUT_OUTER_V_MARGIN, LAYOUT_OUTER_H_MARGIN, LAYOUT_OUTER_V_MARGIN
    );
    hLayout->setSpacing(LAYOUT_SPACING);

    mEdit = new QLineEdit(this);
    mEdit->setObjectName(QStringLiteral("findEdit"));
    mEdit->setPlaceholderText(tr("Find in logs\u2026"));
    mEdit->setClearButtonEnabled(true);
    mEdit->setMinimumWidth(EDIT_MIN_WIDTH);

    hLayout->addWidget(mEdit, /*stretch=*/1);

    // Toggles rendered as flat `QToolButton`s with text glyphs (not
    // `QLineEdit::addAction` icons): `addAction` hides text and the
    // `SP_*` catalogue has no regex/wildcard icon.
    mRegexAction = new QAction(this);
    mRegexAction->setObjectName(QStringLiteral("regexToggle"));
    mRegexAction->setCheckable(true);
    mRegexAction->setText(QStringLiteral(".*"));
    mRegexAction->setToolTip(tr("Match using regular expressions"));

    auto *regexButton = new QToolButton(this);
    regexButton->setObjectName(QStringLiteral("regexToggleButton"));
    regexButton->setDefaultAction(mRegexAction);
    regexButton->setAutoRaise(true);
    regexButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    // Pin the button text explicitly; a few platform styles don't
    // propagate the action's text when the button has none of its own.
    regexButton->setText(mRegexAction->text());
    hLayout->addWidget(regexButton);

    mWildcardsAction = new QAction(this);
    mWildcardsAction->setObjectName(QStringLiteral("wildcardsToggle"));
    mWildcardsAction->setCheckable(true);
    mWildcardsAction->setText(QStringLiteral("*?"));
    mWildcardsAction->setToolTip(tr("Match using wildcards (* and ?)"));

    auto *wildcardsButton = new QToolButton(this);
    wildcardsButton->setObjectName(QStringLiteral("wildcardsToggleButton"));
    wildcardsButton->setDefaultAction(mWildcardsAction);
    wildcardsButton->setAutoRaise(true);
    wildcardsButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    // See `regexButton` above for why we pin the text explicitly.
    wildcardsButton->setText(mWildcardsAction->text());
    hLayout->addWidget(wildcardsButton);

    mMatchCountLabel = new QLabel(this);
    mMatchCountLabel->setObjectName(QStringLiteral("findMatchCount"));
    mMatchCountLabel->setAlignment(Qt::AlignCenter);
    mMatchCountLabel->setMinimumWidth(MATCH_LABEL_MIN_WIDTH);
    // Keep the label slot reserved (just blank) so the arrow buttons
    // don't jitter when the indicator toggles between empty and populated.
    mMatchCountLabel->setText(QString());
    hLayout->addWidget(mMatchCountLabel);

    mButtonPrevious = new QToolButton(this);
    mButtonPrevious->setObjectName(QStringLiteral("findPrevious"));
    mButtonPrevious->setToolTip(tr("Previous match (Shift+Enter)"));
    mButtonPrevious->setAutoRaise(true);
    hLayout->addWidget(mButtonPrevious);

    mButtonNext = new QToolButton(this);
    mButtonNext->setObjectName(QStringLiteral("findNext"));
    mButtonNext->setToolTip(tr("Next match (Enter)"));
    mButtonNext->setAutoRaise(true);
    hLayout->addWidget(mButtonNext);

    RefreshArrowIcons();

    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    setMinimumWidth(WIDGET_MIN_WIDTH);
    // Focus order must name every focusable widget: each `setTabOrder`
    // detaches `b` from its prior slot, so omitting the toggles here
    // would make them keyboard-unreachable.
    setTabOrder(mEdit, regexButton);
    setTabOrder(regexButton, wildcardsButton);
    setTabOrder(wildcardsButton, mButtonPrevious);
    setTabOrder(mButtonPrevious, mButtonNext);
    // No constructor `setFocus`: the host dock isn't realised yet.
    // `SetEditFocus` from `RevealAndFocus` / `Find` does the real work.

    connect(mButtonNext, &QToolButton::clicked, this, &FindRecordWidget::FindNext);
    connect(mButtonPrevious, &QToolButton::clicked, this, &FindRecordWidget::FindPrevious);

    // Return / Shift+Return on the search field are handled in
    // `eventFilter`, not via `returnPressed`: `QLineEdit` emits
    // `returnPressed` then ignores the key, so wiring both would
    // double-advance and skip every other match. `keyPressEvent`
    // still handles Enter when focus is on a toggle / arrow button.
    mEdit->installEventFilter(this);

    // `objectName`s on both timers let tests probe trailing vs max-age
    // individually instead of guessing which `findChild<QTimer*>` is which.
    mMatchCountTimer = new QTimer(this);
    mMatchCountTimer->setObjectName(QStringLiteral("matchCountDebounceTimer"));
    mMatchCountTimer->setSingleShot(true);
    mMatchCountTimer->setInterval(MATCH_COUNT_DEBOUNCE_MS);
    connect(mMatchCountTimer, &QTimer::timeout, this, &FindRecordWidget::EmitMatchCountRequest);

    mMatchCountMaxAgeTimer = new QTimer(this);
    mMatchCountMaxAgeTimer->setObjectName(QStringLiteral("matchCountMaxAgeTimer"));
    mMatchCountMaxAgeTimer->setSingleShot(true);
    mMatchCountMaxAgeTimer->setInterval(MATCH_COUNT_MAX_AGE_MS);
    connect(mMatchCountMaxAgeTimer, &QTimer::timeout, this, &FindRecordWidget::EmitMatchCountRequest);

    connect(mEdit, &QLineEdit::textChanged, this, &FindRecordWidget::RequestMatchCountSoon);
    connect(mWildcardsAction, &QAction::toggled, this, &FindRecordWidget::RequestMatchCountSoon);
    connect(mRegexAction, &QAction::toggled, this, &FindRecordWidget::RequestMatchCountSoon);

    // Regex and wildcards are mutually exclusive in the backend
    // (`LogFilterModel::Matches`); mirror that in the UI so toggling
    // one automatically untoggles the other.
    connect(mRegexAction, &QAction::toggled, this, [this](bool on) {
        if (on && mWildcardsAction->isChecked())
        {
            const QSignalBlocker blocker(mWildcardsAction);
            mWildcardsAction->setChecked(false);
        }
    });
    connect(mWildcardsAction, &QAction::toggled, this, [this](bool on) {
        if (on && mRegexAction->isChecked())
        {
            const QSignalBlocker blocker(mRegexAction);
            mRegexAction->setChecked(false);
        }
    });

    // Escape dismisses the bar via the host dock. Widget-scoped so
    // the shortcut only fires when this widget (or a descendant) has focus.
    auto *escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escapeShortcut, &QShortcut::activated, this, &FindRecordWidget::DismissBar);
}

void FindRecordWidget::SetEditFocus()
{
    mEdit->setFocus();
    mEdit->selectAll();
}

void FindRecordWidget::SetMatchInfo(int current, int total, bool overflowed)
{
    if (total <= 0)
    {
        // Blank label, slot stays reserved (see `MATCH_LABEL_MIN_WIDTH`).
        mMatchCountLabel->clear();
        mMatchCountLabel->setToolTip(QString());
        return;
    }
    // Locale-grouped digits; "+" suffix on `overflowed` signals
    // that `total` is a lower bound (scan may have early-exited
    // once every rail bucket had a hit). The tooltip below spells
    // out the caveat about the position lookup.
    const QLocale locale = QLocale::system();
    const QString totalText =
        locale.toString(static_cast<qlonglong>(total)) + (overflowed ? QStringLiteral("+") : QString());
    QString text;
    if (current > 0)
    {
        text = tr("%1 of %2").arg(locale.toString(static_cast<qlonglong>(current)), totalText);
    }
    else
    {
        // `%Ln` keeps digit grouping consistent with the "%1 of %2"
        // branch above. Two formatters when overflowed because `%Ln`
        // only handles one number and we already formatted `totalText`.
        text = overflowed ? tr("%1 matches").arg(totalText) : tr("%Ln matches", nullptr, total);
    }
    mMatchCountLabel->setText(text);
    // Tooltip is only meaningful under `overflowed` — the "+" alone
    // is ambiguous between "at least N" and "position lookup capped".
    // Clear it otherwise so a stale tooltip doesn't linger after
    // the user narrows the search.
    if (overflowed)
    {
        mMatchCountLabel->setToolTip(tr("Match count is a lower bound (the scan bails once every rail bucket "
                                        "has a hit and the internal cursor cache is full). The current-match "
                                        "index may read as 0 for a match past the cache. The overview rail "
                                        "still shows every bucket that carries a hit."));
    }
    else
    {
        mMatchCountLabel->setToolTip(QString());
    }
}

void FindRecordWidget::BumpMatchCountDebounce()
{
    if (mEdit == nullptr || mEdit->text().isEmpty())
    {
        return;
    }
    // Skip the `start()` call when both timers are already armed.
    // Restarting the trailing timer on every dataChanged during
    // streaming would defeat coalescing and only the max-age timer
    // would force the recount through.
    if (mMatchCountTimer->isActive() && mMatchCountMaxAgeTimer->isActive())
    {
        return;
    }
    mMatchCountTimer->start();
    // Max-age timer arms only on the leading edge so continuous
    // activity can't keep pushing the deadline out.
    if (!mMatchCountMaxAgeTimer->isActive())
    {
        mMatchCountMaxAgeTimer->start();
    }
}

void FindRecordWidget::DismissBar()
{
    // Closing the dock is the only correct dismiss: hiding just the
    // inner widget leaves the dock framed around an empty body, and
    // a later `show()` on the dock won't un-hide an explicitly-hidden
    // child. Walk the parent chain in case the dock framework ever
    // introduces an intermediate container.
    for (QWidget *p = parentWidget(); p != nullptr; p = p->parentWidget())
    {
        if (auto *dock = qobject_cast<QDockWidget *>(p))
        {
            dock->close();
            return;
        }
    }
    // Production never hits this (always parented under `FindDock`),
    // but a test or future embedding could. Surface the no-op so a
    // silent Escape isn't mistaken for a working one.
    qWarning() << "FindRecordWidget::DismissBar: no QDockWidget ancestor; Escape is a no-op in this configuration";
}

void FindRecordWidget::keyPressEvent(QKeyEvent *event)
{
    // Only reaches us when focus is on a toggle / arrow button;
    // `eventFilter` consumes Return / Shift+Return on `mEdit`.
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
    {
        if (event->modifiers().testFlag(Qt::ShiftModifier))
        {
            FindPrevious();
        }
        else
        {
            FindNext();
        }
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}

bool FindRecordWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == mEdit && event->type() == QEvent::KeyPress)
    {
        // `QEvent::KeyPress` guarantees the dynamic type; Qt doesn't
        // enable RTTI on `QEvent`.
        auto *ke = static_cast<QKeyEvent *>(event); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter)
        {
            // Consume before `QLineEdit::keyPressEvent` so it
            // can't bubble into `keyPressEvent` and double-fire
            // FindNext. Shift+Return → find-prev; plain Return
            // → find-next.
            if (ke->modifiers().testFlag(Qt::ShiftModifier))
            {
                FindPrevious();
            }
            else
            {
                FindNext();
            }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void FindRecordWidget::FindNext()
{
    if (mEdit->text().isEmpty())
    {
        return;
    }
    emit FindRecords(mEdit->text(), true, mWildcardsAction->isChecked(), mRegexAction->isChecked());
}

void FindRecordWidget::FindPrevious()
{
    if (mEdit->text().isEmpty())
    {
        return;
    }
    emit FindRecords(mEdit->text(), false, mWildcardsAction->isChecked(), mRegexAction->isChecked());
}

void FindRecordWidget::RequestMatchCountSoon()
{
    if (mEdit->text().isEmpty())
    {
        // Empty needle: clear the label immediately, cancel any
        // in-flight debounce, and signal the parent so it can drop
        // dependent match state (find cache + overview-rail ticks).
        // Without the emit a cleared find bar would leave the last
        // needle's ticks stranded on the rail.
        mMatchCountTimer->stop();
        mMatchCountMaxAgeTimer->stop();
        SetMatchInfo(0, 0);
        emit MatchCountRequested(mEdit->text(), mWildcardsAction->isChecked(), mRegexAction->isChecked());
        return;
    }
    mMatchCountTimer->start();
    if (!mMatchCountMaxAgeTimer->isActive())
    {
        mMatchCountMaxAgeTimer->start();
    }
}

void FindRecordWidget::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event == nullptr)
    {
        return;
    }
    // Both events can fire on a theme switch (PaletteChange when
    // colours roll into `palette()`, StyleChange when the theme
    // also swaps `qApp->style()`); `RefreshArrowIcons` is idempotent.
    const QEvent::Type type = event->type();
    if (type == QEvent::PaletteChange || type == QEvent::StyleChange || type == QEvent::ApplicationPaletteChange)
    {
        RefreshArrowIcons();
    }
}

bool FindRecordWidget::event(QEvent *event)
{
    // Re-mint at the new DPR when the bar moves between monitors
    // with different scale factors; otherwise Qt up-scales the
    // pixmap allocated at the previous DPR.
    if (event != nullptr && event->type() == QEvent::DevicePixelRatioChange)
    {
        RefreshArrowIcons();
    }
    return QWidget::event(event);
}

void FindRecordWidget::RefreshArrowIcons()
{
    if (mButtonPrevious == nullptr || mButtonNext == nullptr)
    {
        return;
    }
    // `WindowText` (not `ButtonText`): these are autoRaise toolbuttons
    // whose backdrop is the find-bar surface, not a raised button face.
    // Matches the colour of the `.*` / `*?` toggle glyphs.
    const QColor color = palette().color(QPalette::Active, QPalette::WindowText);
    const int sizePx = ArrowIconPixels(this);
    const qreal dpr = devicePixelRatioF();
    mButtonPrevious->setIcon(MakeArrowIcon(sizePx, /*pointingUp=*/true, color, dpr));
    mButtonNext->setIcon(MakeArrowIcon(sizePx, /*pointingUp=*/false, color, dpr));
}

void FindRecordWidget::EmitMatchCountRequest()
{
    // Re-entrancy guard: both timers route here, and if both
    // `timeout` events landed in the queue on the same pass, the
    // first invocation's `stop()` calls can't undo the queued second.
    if (mEmittingMatchCountRequest)
    {
        return;
    }
    mEmittingMatchCountRequest = true;
    mMatchCountTimer->stop();
    mMatchCountMaxAgeTimer->stop();
    emit MatchCountRequested(mEdit->text(), mWildcardsAction->isChecked(), mRegexAction->isChecked());
    mEmittingMatchCountRequest = false;
}
