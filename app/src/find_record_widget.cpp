#include "find_record_widget.hpp"

#include <QAction>
#include <QApplication>
#include <QColor>
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
/// Debounce window after a key press before we ask the parent to
/// recount matches. Avoids a full-table scan on every keystroke
/// of a long word.
constexpr int MATCH_COUNT_DEBOUNCE_MS = 120;

/// Hard cap on how long a match-count emit can be deferred. The
/// trailing-edge debounce restarts on every keystroke / model
/// bump, which under continuous activity (live-tail streaming,
/// long words held down) means it would never fire and the "*i*
/// of *N*" indicator would lag the live data by minutes. The
/// max-age timer is armed once when the trailing debounce first
/// starts and is *not* restarted, so it guarantees an emit
/// within this window even when the trailing timer keeps
/// resetting.
constexpr int MATCH_COUNT_MAX_AGE_MS = 750;

/// Visual minimum so the search field doesn't collapse to nothing
/// when the bar is squeezed.
constexpr int EDIT_MIN_WIDTH = 220;

/// Floor for the find bar as a whole. Drives `setMinimumWidth` on
/// the widget so a floating `FindDock` can't collapse below this
/// (the line edit's own minimum protects only the inner layout,
/// not the dock's outer frame). Sized to fit the line edit, both
/// toggles, the count label, and both arrow buttons without
/// truncation.
constexpr int WIDGET_MIN_WIDTH = 360;

/// Outer margins / spacing for the find bar's horizontal layout.
/// Matches the densities of the other docks (`anchors_dock`,
/// `record_detail_widget`).
constexpr int LAYOUT_OUTER_H_MARGIN = 6;
constexpr int LAYOUT_OUTER_V_MARGIN = 4;
constexpr int LAYOUT_SPACING = 4;

/// Reserved width for the "i of N" label so the count digits
/// don't shift the arrow buttons left/right as totals grow.
constexpr int MATCH_LABEL_MIN_WIDTH = 80;

/// Edge length for the painted arrow icons. Falls back to this
/// when `QStyle::PM_SmallIconSize` returns nonsense (offscreen
/// QPA with no style hint table).
constexpr int ARROW_ICON_FALLBACK_PX = 14;

/// Fraction of the icon edge to inset the triangle from each
/// side. Tuned by eye to match the visual weight of the
/// surrounding `.*` / `*?` toggle glyphs.
constexpr qreal ARROW_INSET_RATIO = 0.18;

/// Centre of the icon expressed as a fraction of the edge.
constexpr qreal ARROW_CENTRE_RATIO = 0.5;

/// Build a small chevron-style arrow icon painted in @p color so
/// the glyph follows the active palette / theme. `pointingUp =
/// true` mints the previous-match arrow (apex at the top), false
/// the next-match arrow (apex at the bottom). Replaces
/// `QStyle::SP_ArrowUp` / `SP_ArrowDown`, which Qt bakes as raster
/// black-on-transparent pixmaps that vanish on dark themes.
[[nodiscard]] QIcon MakeArrowIcon(int sizePx, bool pointingUp, const QColor &color)
{
    QPixmap pix(sizePx, sizePx);
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

    // Regex / wildcard toggles. Rendered as small flat `QToolButton`s
    // beside the line edit (not `QLineEdit::addAction` icons) because:
    //   - we want the glyphs ".*" and "*?" visible, but
    //     `QLineEdit::addAction` only renders the action's icon and
    //     hides the text; and
    //   - Qt's `QStyle::SP_*` icon catalogue has no recognisable
    //     regex / wildcard glyph, so falling back to file-dialog
    //     icons (as the previous version did) reads as a file-view
    //     toggle and misleads users about what the toggle does.
    // `setAutoRaise(true)` matches the find-next / find-prev
    // buttons' flat look.
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
    // Defensive: `setDefaultAction` propagates the action's text in
    // most styles, but a few platform styles only render text when
    // `QToolButton` has its own. Setting it explicitly pins the
    // glyph regardless of style; click handling continues to flow
    // through the action.
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
    // Same rationale as `regexButton` above.
    wildcardsButton->setText(mWildcardsAction->text());
    hLayout->addWidget(wildcardsButton);

    mMatchCountLabel = new QLabel(this);
    mMatchCountLabel->setObjectName(QStringLiteral("findMatchCount"));
    mMatchCountLabel->setAlignment(Qt::AlignCenter);
    mMatchCountLabel->setMinimumWidth(MATCH_LABEL_MIN_WIDTH);
    // Always present in the layout (just empty) so the arrow
    // buttons keep their horizontal position when the indicator
    // toggles between "no needle" and "12 matches"; otherwise
    // they jump left/right on every keystroke that crosses the
    // empty boundary.
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

    // Paint the arrow glyphs from the palette so they remain
    // visible after a Light <-> Dark theme switch. The default
    // `QStyle::SP_ArrowUp` / `SP_ArrowDown` pixmaps are baked
    // black on Windows and vanish on a dark background; our own
    // icons follow `QPalette::WindowText`. `changeEvent` re-runs
    // this when the application palette / style updates.
    RefreshArrowIcons();

    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    // Width floor so a floating `FindDock` (the dock has no
    // minimum of its own) can't be dragged narrow enough to
    // truncate the toggles or the count label. The line edit's
    // `EDIT_MIN_WIDTH` covers the layout's inner shrinking, not
    // the outer frame.
    setMinimumWidth(WIDGET_MIN_WIDTH);
    // Focus order: line edit -> regex toggle -> wildcards toggle ->
    // previous -> next. Each `setTabOrder(a, b)` re-inserts `b`
    // immediately after `a` and detaches it from its prior slot --
    // so the chain has to name *every* focusable widget, not just
    // the endpoints, otherwise the unnamed siblings get severed
    // from the chain and become unreachable via Tab. (Earlier
    // versions of this widget chained only `mEdit -> previous ->
    // next`, which made the regex/wildcards toggles keyboard-
    // unreachable.)
    setTabOrder(mEdit, regexButton);
    setTabOrder(regexButton, wildcardsButton);
    setTabOrder(wildcardsButton, mButtonPrevious);
    setTabOrder(mButtonPrevious, mButtonNext);
    // No constructor `setFocus`: the host (dock or layout) is not
    // realised yet, so the call is a no-op. Real focus is granted
    // by `SetEditFocus` from `RevealAndFocus` / `Find`.

    connect(mButtonNext, &QToolButton::clicked, this, &FindRecordWidget::FindNext);
    connect(mButtonPrevious, &QToolButton::clicked, this, &FindRecordWidget::FindPrevious);

    // Plain Return -> find-next via `returnPressed`. Shift+Return
    // -> find-previous is wired through `eventFilter` below;
    // `QLineEdit::keyPressEvent` accepts Return for itself, so the
    // event never bubbles to our `keyPressEvent`, and
    // `returnPressed` carries no modifier state.
    connect(mEdit, &QLineEdit::returnPressed, this, &FindRecordWidget::FindNext);
    mEdit->installEventFilter(this);

    // Owned single-shot timer; `start()` restarts it on each
    // keystroke so multi-keystroke edits coalesce into one
    // `MatchCountRequested` emit. `QTimer::singleShot` does not
    // coalesce because each call schedules an independent fire.
    mMatchCountTimer = new QTimer(this);
    mMatchCountTimer->setSingleShot(true);
    mMatchCountTimer->setInterval(MATCH_COUNT_DEBOUNCE_MS);
    connect(mMatchCountTimer, &QTimer::timeout, this, &FindRecordWidget::EmitMatchCountRequest);

    // Companion max-age timer: armed alongside `mMatchCountTimer`
    // (only when the trailing timer wasn't already running) and
    // *never* restarted by subsequent bumps. Forces an emit after
    // at most `MATCH_COUNT_MAX_AGE_MS`, so live-tail streaming or
    // a held-down key can't strand the "*i* of *N*" indicator at
    // its pre-activity value.
    mMatchCountMaxAgeTimer = new QTimer(this);
    mMatchCountMaxAgeTimer->setSingleShot(true);
    mMatchCountMaxAgeTimer->setInterval(MATCH_COUNT_MAX_AGE_MS);
    connect(mMatchCountMaxAgeTimer, &QTimer::timeout, this, &FindRecordWidget::EmitMatchCountRequest);

    // Text + toggle changes drive live match-count requests. We
    // debounce via `mMatchCountTimer` so a fast typist doesn't
    // trigger a full-table scan on every keystroke.
    connect(mEdit, &QLineEdit::textChanged, this, &FindRecordWidget::RequestMatchCountSoon);
    connect(mWildcardsAction, &QAction::toggled, this, &FindRecordWidget::RequestMatchCountSoon);
    connect(mRegexAction, &QAction::toggled, this, &FindRecordWidget::RequestMatchCountSoon);

    // Regex and wildcards are mutually exclusive match modes in the
    // backend (`LogFilterModel::Matches` treats them as alternatives,
    // not modifiers). Mirror that in the UI so toggling one
    // automatically untoggles the other instead of leaving the user
    // with two enabled-looking toggles where only one actually takes
    // effect.
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

    // Escape dismisses the bar. Scope `WidgetWithChildrenShortcut`
    // so the shortcut fires whenever this widget (or any
    // descendant focusable) has focus. `DismissBar` closes the
    // host `QDockWidget` so its `visibilityChanged` mirrors the
    // toggle action and a subsequent `RevealAndFocus` properly
    // re-shows everything.
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
        // Keep the label slot in the layout but blank, so the
        // arrow buttons don't shift left/right as the indicator
        // appears and disappears around each keystroke that
        // empties or refills the search field. The reserved
        // `MATCH_LABEL_MIN_WIDTH` continues to hold the slot
        // open.
        mMatchCountLabel->clear();
        return;
    }
    // Locale-grouped digits matching the rest of the GUI; the "+"
    // suffix surfaces the parent's hit cap so a million-row log
    // doesn't pretend the count is exactly at the cap.
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
        // Two formatters because `%n` / `%Ln` only handles one
        // number; we already formatted `totalText` above.
        // `%Ln` (not `%n`) keeps digit grouping consistent with
        // the "%1 of %2" branch above and with
        // `ParseErrorsDock::RefreshSummary` -- otherwise a count
        // of 10000 would render unstyled while the next-click
        // refresh would render `1 of 10,000`.
        text = overflowed ? tr("%1 matches").arg(totalText) : tr("%Ln matches", nullptr, total);
    }
    mMatchCountLabel->setText(text);
}

void FindRecordWidget::BumpMatchCountDebounce()
{
    if (mEdit == nullptr || mEdit->text().isEmpty())
    {
        return;
    }
    // Restart the trailing timer; identical mechanism to the
    // textChanged path. A burst of model signals collapses into
    // one final recount.
    //
    // Arm the max-age timer only when it isn't already running,
    // so continuous activity can't keep pushing the deadline
    // out -- it caps the longest possible delay between an
    // invalidation and the visible recount.
    mMatchCountTimer->start();
    if (!mMatchCountMaxAgeTimer->isActive())
    {
        mMatchCountMaxAgeTimer->start();
    }
}

void FindRecordWidget::DismissBar()
{
    // Walk up to the host `QDockWidget` and close it. Closing the
    // dock is the only correct dismiss: hiding only the inner
    // widget leaves the dock title bar floating over an empty
    // body, and a later `show()` on the dock will not un-hide an
    // explicitly-hidden child. Walking the parent chain (instead
    // of asserting a single hop) keeps this resilient to any
    // future intermediate container the dock framework introduces.
    for (QWidget *p = parentWidget(); p != nullptr; p = p->parentWidget())
    {
        if (auto *dock = qobject_cast<QDockWidget *>(p))
        {
            dock->close();
            return;
        }
    }
}

void FindRecordWidget::keyPressEvent(QKeyEvent *event)
{
    // Reachable only when focus is on the bar but not on `mEdit`
    // (the toggle / arrow buttons). For `mEdit`, the line edit
    // accepts Return for itself and the modifier-bearing variant
    // is handled in `eventFilter`. Handling it here too keeps the
    // VS Code / Chromium convention for the rest of the bar.
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
        // `QEvent::KeyPress` guarantees the dynamic type; Qt does
        // not enable RTTI on `QEvent`. Same idiom as the
        // `QFileOpenEvent` cast in `main.cpp`.
        auto *ke = static_cast<QKeyEvent *>(event); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        if ((ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) &&
            ke->modifiers().testFlag(Qt::ShiftModifier))
        {
            // Intercept before `QLineEdit::keyPressEvent` runs.
            // Otherwise it would emit `returnPressed` (wired to
            // `FindNext`) and accept the event, swallowing the
            // shift-modified variant.
            FindPrevious();
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
        // Empty needle: clear immediately without bouncing off
        // the debounce timer, so the label can't lag a clear.
        // Cancel any in-flight tick (trailing + max-age) so a
        // stale needle doesn't overwrite the cleared state. No
        // signal emitted -- the parent has nothing to recount,
        // and a per-keystroke round-trip just to be told "still
        // empty" is wasted work. The cache the parent holds is
        // keyed by needle, so the next non-empty query rebuilds
        // it anyway.
        mMatchCountTimer->stop();
        mMatchCountMaxAgeTimer->stop();
        SetMatchInfo(0, 0);
        return;
    }
    // `start()` resets the countdown if already running, so a
    // fast typist coalesces N keystrokes into a single trailing
    // match-count scan. The max-age timer arms once on the
    // leading edge so it caps the longest possible delay even
    // under sustained typing.
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
    // Both events fire on a theme switch:
    //   PaletteChange  - new colours rolled into `palette()`
    //   StyleChange    - theme also swapped `qApp->style()`
    // Either one is sufficient to mint stale icons, so handle
    // both. The repaint is cheap (two 14x14 triangles) and
    // `RefreshArrowIcons` is idempotent.
    const QEvent::Type type = event->type();
    if (type == QEvent::PaletteChange || type == QEvent::StyleChange || type == QEvent::ApplicationPaletteChange)
    {
        RefreshArrowIcons();
    }
}

void FindRecordWidget::RefreshArrowIcons()
{
    if (mButtonPrevious == nullptr || mButtonNext == nullptr)
    {
        return;
    }
    // Pull `WindowText` rather than `ButtonText`: the buttons are
    // `autoRaise` toolbuttons whose backdrop matches the
    // surrounding find-bar surface, not a raised button face, so
    // `WindowText` is the contrast colour the user actually sees
    // these glyphs against. Matches the colour the `.*` / `*?`
    // toggle action glyphs render in.
    const QColor color = palette().color(QPalette::Active, QPalette::WindowText);
    const int sizePx = ArrowIconPixels(this);
    mButtonPrevious->setIcon(MakeArrowIcon(sizePx, /*pointingUp=*/true, color));
    mButtonNext->setIcon(MakeArrowIcon(sizePx, /*pointingUp=*/false, color));
}

void FindRecordWidget::EmitMatchCountRequest()
{
    // Stop the *other* timer so a max-age fire doesn't get
    // followed 50 ms later by a redundant trailing fire (and
    // vice versa). The re-entrancy guard catches the harder
    // case: when both timers' `timeout` events landed in the
    // queue on the same pass, stopping them here doesn't undo
    // the second event that's already queued -- but the guard
    // collapses it.
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
