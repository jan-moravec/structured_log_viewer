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
    // Leading magnifying-glass affordance. Decorative only -- the
    // leading-position icon in `QLineEdit` does not emit
    // `triggered` on click, so wiring a slot is dead code.
    mEdit->addAction(
        QApplication::style()->standardIcon(QStyle::SP_FileDialogContentsView), QLineEdit::LeadingPosition
    );

    // Trailing toggle "buttons" embedded in the line edit. `addAction`
    // with `TrailingPosition` renders them as small icon buttons
    // inside the field, before the clear button (Qt orders them in
    // insertion order from the inner edge outward).
    mRegexAction = new QAction(this);
    mRegexAction->setObjectName(QStringLiteral("regexToggle"));
    mRegexAction->setCheckable(true);
    mRegexAction->setText(QStringLiteral(".*"));
    mRegexAction->setIcon(QApplication::style()->standardIcon(QStyle::SP_FileDialogDetailedView));
    mRegexAction->setToolTip(tr("Match using regular expressions"));
    mEdit->addAction(mRegexAction, QLineEdit::TrailingPosition);

    mWildcardsAction = new QAction(this);
    mWildcardsAction->setObjectName(QStringLiteral("wildcardsToggle"));
    mWildcardsAction->setCheckable(true);
    mWildcardsAction->setText(QStringLiteral("*?"));
    mWildcardsAction->setIcon(QApplication::style()->standardIcon(QStyle::SP_FileDialogListView));
    mWildcardsAction->setToolTip(tr("Match using wildcards (* and ?)"));
    mEdit->addAction(mWildcardsAction, QLineEdit::TrailingPosition);

    hLayout->addWidget(mEdit, /*stretch=*/1);

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
    // Shift+Return on `mEdit` is handled in `eventFilter` (the
    // line edit accepts the event itself, so it never bubbles
    // here). This override only catches Shift+Return / Return
    // when focus is on the find bar's buttons or the bar widget
    // itself -- a rare path, but keep the convention.
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
    {
        if (event->modifiers().testFlag(Qt::ShiftModifier))
        {
            FindPrevious();
            event->accept();
            return;
        }
        FindNext();
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
