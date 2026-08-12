#include "find_dock.hpp"

#include "find_record_widget.hpp"
#include "log_session.hpp"
#include "session_bind_context.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QShowEvent>
#include <QWidget>

FindDock::FindDock(QWidget *parent)
    : QDockWidget(tr("Find"), parent)
{
    setObjectName(QStringLiteral("findDock"));
    // Find bars belong on the top or bottom edge; a vertical side dock
    // would squeeze the search field into a useless narrow column.
    setAllowedAreas(Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    mWidget = new FindRecordWidget(this);
    setWidget(mWidget);
}

void FindDock::RevealAndFocus()
{
    // Refresh the stash on every reveal whose source focus is outside
    // our subtree, so Ctrl+F invoked while the bar is already on-screen
    // (but focus has drifted back to the table) picks up the current
    // target instead of a stale one from the very first reveal.
    QWidget *current = QApplication::focusWidget();
    if (current != nullptr && !isAncestorOf(current))
    {
        mFocusBeforeReveal = current;
    }
    if (!isVisible())
    {
        show();
    }
    raise();
    mWidget->SetEditFocus();
}

void FindDock::closeEvent(QCloseEvent *event)
{
    // Snapshot + clear before forwarding so a re-entrant close (e.g.
    // a focusOut handler that triggers another close) can't double-fire
    // the restore.
    QWidget *target = mFocusBeforeReveal.data();
    mFocusBeforeReveal.clear();
    QDockWidget::closeEvent(event);
    if (!event->isAccepted())
    {
        return;
    }
    if (target != nullptr && target->isVisible())
    {
        target->setFocus(Qt::OtherFocusReason);
    }
    emit closed();
}

void FindDock::showEvent(QShowEvent *event)
{
    QDockWidget::showEvent(event);
    emit revealed();
}

void FindDock::Bind(const SessionBindContext &context)
{
    LogSession *outgoing = mBoundSession.data();
    LogSession *incoming = context.session.data();

    // Same-session short-circuit: a redundant Bind of the same
    // session is a no-op (shadow is already in sync with the
    // session's log). Both sides must be non-null: a null==null
    // match happens when the previously-bound session was
    // destroyed (QPointer auto-nulls `mBoundSession`) and then
    // `Bind(MakeUnbound())` is called from the teardown path.
    // Origin-review finding M7: without the non-null clause the
    // guard would return here and leave the bar showing the
    // destroyed session's query text + toggle state.
    if (outgoing != nullptr && outgoing == incoming)
    {
        return;
    }

    // Save-outgoing: capture the visible query state into the
    // outgoing session's store.
    if (outgoing != nullptr && mWidget != nullptr)
    {
        SessionFindQueryState &state = outgoing->MutableFindQuery();
        state.query = mWidget->queryText();
        state.wildcards = mWidget->queryWildcards();
        state.regex = mWidget->queryRegex();
    }

    // Cancel any in-flight debounce so the timer cannot fire a
    // `MatchCountRequested` against a stale model between the
    // clear below and the debounce re-arm on restore.
    if (mWidget != nullptr)
    {
        mWidget->CancelPendingMatchCountRequest();
    }

    mBoundSession = incoming;

    // Load-incoming: restore the incoming session's query state
    // (or clear when the incoming context is unbound).
    if (mWidget != nullptr)
    {
        if (incoming != nullptr)
        {
            const SessionFindQueryState &state = incoming->FindQuery();
            mWidget->RestoreQueryState(state.query, state.wildcards, state.regex);
        }
        else
        {
            mWidget->RestoreQueryState({}, /*wildcards=*/false, /*regex=*/false);
        }
    }
}

void FindDock::Unbind()
{
    // Equivalent to `Bind(SessionBindContext::MakeUnbound())` with
    // fewer branches for teardown call sites that already know they
    // want the empty state.
    if (LogSession *session = mBoundSession.data(); session != nullptr && mWidget != nullptr)
    {
        SessionFindQueryState &state = session->MutableFindQuery();
        state.query = mWidget->queryText();
        state.wildcards = mWidget->queryWildcards();
        state.regex = mWidget->queryRegex();
    }
    if (mWidget != nullptr)
    {
        mWidget->CancelPendingMatchCountRequest();
        mWidget->RestoreQueryState({}, /*wildcards=*/false, /*regex=*/false);
    }
    mBoundSession = nullptr;
}
