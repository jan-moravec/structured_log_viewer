#include "find_record_widget.hpp"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
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

/// Visual minimum so the search field doesn't collapse to nothing
/// when the bar is squeezed.
constexpr int EDIT_MIN_WIDTH = 220;

/// Outer margins / spacing for the find bar's horizontal layout.
/// Matches the densities of the other docks (`anchors_dock`,
/// `record_detail_widget`).
constexpr int LAYOUT_OUTER_H_MARGIN = 6;
constexpr int LAYOUT_OUTER_V_MARGIN = 4;
constexpr int LAYOUT_SPACING = 4;

/// Reserved width for the "i of N" label so the count digits
/// don't shift the arrow buttons left/right as totals grow.
constexpr int MATCH_LABEL_MIN_WIDTH = 80;
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
    hLayout->addWidget(wildcardsButton);

    mMatchCountLabel = new QLabel(this);
    mMatchCountLabel->setObjectName(QStringLiteral("findMatchCount"));
    mMatchCountLabel->setAlignment(Qt::AlignCenter);
    mMatchCountLabel->setMinimumWidth(MATCH_LABEL_MIN_WIDTH);
    mMatchCountLabel->setVisible(false);
    hLayout->addWidget(mMatchCountLabel);

    mButtonPrevious = new QToolButton(this);
    mButtonPrevious->setObjectName(QStringLiteral("findPrevious"));
    mButtonPrevious->setIcon(QApplication::style()->standardIcon(QStyle::SP_ArrowUp));
    mButtonPrevious->setToolTip(tr("Previous match (Shift+Enter)"));
    mButtonPrevious->setAutoRaise(true);
    hLayout->addWidget(mButtonPrevious);

    mButtonNext = new QToolButton(this);
    mButtonNext->setObjectName(QStringLiteral("findNext"));
    mButtonNext->setIcon(QApplication::style()->standardIcon(QStyle::SP_ArrowDown));
    mButtonNext->setToolTip(tr("Next match (Enter)"));
    mButtonNext->setAutoRaise(true);
    hLayout->addWidget(mButtonNext);

    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Preferred);
    // Focus order: line edit first; toggles + arrow buttons stay
    // reachable via Tab once focus is in the bar.
    setTabOrder(mEdit, mButtonPrevious);
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

void FindRecordWidget::SetMatchInfo(int current, int total)
{
    if (total <= 0)
    {
        mMatchCountLabel->clear();
        mMatchCountLabel->setVisible(false);
        return;
    }
    QString text;
    if (current > 0)
    {
        text = tr("%1 of %2").arg(current).arg(total);
    }
    else
    {
        text = tr("%n matches", nullptr, total);
    }
    mMatchCountLabel->setText(text);
    mMatchCountLabel->setVisible(true);
}

void FindRecordWidget::BumpMatchCountDebounce()
{
    if (mEdit == nullptr || mEdit->text().isEmpty())
    {
        return;
    }
    // Restart the timer; identical mechanism to the textChanged
    // path. A burst of model signals collapses into one final
    // recount.
    mMatchCountTimer->start();
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
        // Cancel any in-flight tick so a stale needle doesn't
        // overwrite the cleared state.
        mMatchCountTimer->stop();
        SetMatchInfo(0, 0);
        emit MatchCountRequested(QString(), mWildcardsAction->isChecked(), mRegexAction->isChecked());
        return;
    }
    // `start()` resets the countdown if already running, so a
    // fast typist coalesces N keystrokes into a single trailing
    // match-count scan.
    mMatchCountTimer->start();
}

void FindRecordWidget::EmitMatchCountRequest()
{
    emit MatchCountRequested(mEdit->text(), mWildcardsAction->isChecked(), mRegexAction->isChecked());
}
