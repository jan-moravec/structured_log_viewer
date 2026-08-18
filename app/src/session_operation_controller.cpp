#include "session_operation_controller.hpp"

#include "anchor_manager.hpp"
#include "highlight_rule_set.hpp"
#include "log_filter_model.hpp"
#include "log_session.hpp"
#include "log_session_view.hpp"
#include "log_table_view.hpp"
#include "main_window.hpp"
#include "row_order_proxy_model.hpp"

SessionOperationController::SessionOperationController(MainWindow &window) noexcept
    : mWindow(window)
{
}

SessionOperationTarget SessionOperationController::TargetFor(LogSession *origin) const noexcept
{
    SessionOperationTarget target;
    if (origin == nullptr || mWindow.HostedSession(origin->InstanceId()) != origin)
    {
        return target;
    }
    target.session = origin;
    target.view = mWindow.ViewAtTab(mWindow.TabIndexForSession(origin->InstanceId()));
    target.model = origin->Model();
    target.rowOrder = origin->RowOrderProxy();
    target.filter = origin->FilterProxy();
    target.anchors = origin->Anchors();
    target.highlights = origin->Highlights();
    if (target.view != nullptr)
    {
        target.table = target.view->TableView();
    }
    target.isActive = origin == mWindow.activeSession();
    return target;
}

void SessionOperationController::CompleteStreaming(LogSession *origin, StreamingResult result)
{
    if (!TargetFor(origin).isValid())
    {
        return;
    }
    mWindow.OnStreamingFinished(origin, result);
}

void SessionOperationController::CompleteDecompression(LogSession *origin)
{
    if (!TargetFor(origin).isValid())
    {
        return;
    }
    mWindow.OnDecompressionFinishedFor(origin);
}

void SessionOperationController::CompleteExport(LogSession *origin)
{
    mWindow.OnExportFinishedFor(origin);
}

bool SessionOperationController::SaveSnapshot(LogSession *origin, bool publishOpenWindow)
{
    return mWindow.AutoSaveSessionSnapshot(origin, publishOpenWindow);
}

void SessionOperationController::SaveAllHostedSnapshots(bool publishOpenWindow)
{
    for (LogSession *session : mWindow.hostedSessions())
    {
        (void)SaveSnapshot(session, publishOpenWindow);
    }
}
