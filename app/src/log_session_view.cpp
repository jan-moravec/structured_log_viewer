#include "log_session_view.hpp"

#include "log_session.hpp"

LogSessionView::LogSessionView(LogSession *session, QWidget *parent)
    : QWidget(parent), mSession(session)
{
    // Phase 1 skeleton: the table, overview rail, delegates,
    // selection, and follow-newest state migrate in Phase 3 (see
    // `tasks/tasks-main-window-session-tabs.md` §3.1). Keep the
    // widget lifetime bound to `mSession` — a null session is a
    // programming error in production, so the assertion helps
    // tests fail early rather than lazily on the first paint.
    Q_ASSERT_X(mSession != nullptr, "LogSessionView", "session must not be null");
}

LogSessionView::~LogSessionView() = default;
