#include "find_record_widget.hpp"

#include <QAction>
#include <QApplication>
#include <QEasingCurve>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPropertyAnimation>
#include <QShortcut>
#include <QShowEvent>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QToolButton>

#include <algorithm>

namespace
{
/// Slide-in / slide-out duration. Slow enough to read, fast
/// enough not to feel sluggish; matches VS Code's find bar.
constexpr int FIND_REVEAL_ANIMATION_MS = 120;

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
    // Leading magnifying-glass affordance. The action is decorative
    // only; clicking it focuses the field (idempotent, since the
    // user just clicked into the field).
    auto *searchIconAction = mEdit->addAction(
        QApplication::style()->standardIcon(QStyle::SP_FileDialogContentsView), QLineEdit::LeadingPosition
    );
    connect(searchIconAction, &QAction::triggered, mEdit, qOverload<>(&QWidget::setFocus));

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

    mEdit->setFocus();
    QTimer::singleShot(0, mEdit, qOverload<>(&QWidget::setFocus));

    connect(mButtonNext, &QToolButton::clicked, this, &FindRecordWidget::FindNext);
    connect(mButtonPrevious, &QToolButton::clicked, this, &FindRecordWidget::FindPrevious);

    // Return triggers find-next; Shift+Return is wired through
    // `keyPressEvent` because `returnPressed` does not carry
    // modifier state.
    connect(mEdit, &QLineEdit::returnPressed, this, &FindRecordWidget::FindNext);

    // Text + toggle changes drive live match-count requests. We
    // debounce so a fast typist doesn't trigger a full-table scan
    // on every keystroke.
    connect(mEdit, &QLineEdit::textChanged, this, &FindRecordWidget::RequestMatchCountSoon);
    connect(mWildcardsAction, &QAction::toggled, this, &FindRecordWidget::RequestMatchCountSoon);
    connect(mRegexAction, &QAction::toggled, this, &FindRecordWidget::RequestMatchCountSoon);

    // Escape hides the bar instead of an explicit "X" button.
    // Scope `WindowShortcut` so the shortcut fires whenever this
    // widget (or any descendant focusable) has focus.
    auto *escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escapeShortcut, &QShortcut::activated, this, &FindRecordWidget::HideAnimated);
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

void FindRecordWidget::RevealAnimated()
{
    if (mNaturalHeight == 0)
    {
        // `sizeHint` is the best estimate before the first paint
        // delivers a real geometry; tracked in `showEvent` once
        // we know.
        mNaturalHeight = sizeHint().height();
    }
    setMaximumHeight(mNaturalHeight);
    show();

    if (mAnimation && mAnimation->state() == QAbstractAnimation::Running)
    {
        mAnimation->stop();
    }
    auto *animation = new QPropertyAnimation(this, "maximumHeight", this);
    animation->setDuration(FIND_REVEAL_ANIMATION_MS);
    animation->setStartValue(0);
    animation->setEndValue(mNaturalHeight);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    animation->start(QAbstractAnimation::DeleteWhenStopped);
    mAnimation = animation;
}

void FindRecordWidget::HideAnimated()
{
    if (!isVisible())
    {
        return;
    }
    const int from = height();
    if (mAnimation && mAnimation->state() == QAbstractAnimation::Running)
    {
        mAnimation->stop();
    }
    auto *animation = new QPropertyAnimation(this, "maximumHeight", this);
    animation->setDuration(FIND_REVEAL_ANIMATION_MS);
    animation->setStartValue(from);
    animation->setEndValue(0);
    animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(animation, &QPropertyAnimation::finished, this, [this]() {
        hide();
        // Reset the cap so a subsequent `show()` (e.g. parent
        // calls `show()` directly instead of `RevealAnimated`)
        // does not stay collapsed.
        setMaximumHeight(QWIDGETSIZE_MAX);
    });
    animation->start(QAbstractAnimation::DeleteWhenStopped);
    mAnimation = animation;
}

void FindRecordWidget::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
    {
        if (event->modifiers().testFlag(Qt::ShiftModifier))
        {
            FindPrevious();
            event->accept();
            return;
        }
        // Plain Return is already wired through `returnPressed`
        // on the line edit; fall through so the default still
        // works for buttons that have focus.
    }
    QWidget::keyPressEvent(event);
}

void FindRecordWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    // Capture the real expanded height the first time we are
    // realized so `RevealAnimated` has a sane target on later
    // toggles.
    mNaturalHeight = std::max(mNaturalHeight, sizeHint().height());
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
        SetMatchInfo(0, 0);
        emit MatchCountRequested(QString(), mWildcardsAction->isChecked(), mRegexAction->isChecked());
        return;
    }
    // Debounce so multi-keystroke edits coalesce into one count
    // request. `singleShot` against `this` so destruction cancels
    // the pending fire.
    QTimer::singleShot(MATCH_COUNT_DEBOUNCE_MS, this, &FindRecordWidget::EmitMatchCountRequest);
}

void FindRecordWidget::EmitMatchCountRequest()
{
    emit MatchCountRequested(mEdit->text(), mWildcardsAction->isChecked(), mRegexAction->isChecked());
}
