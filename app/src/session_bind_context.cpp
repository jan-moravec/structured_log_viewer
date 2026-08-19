#include "session_bind_context.hpp"

#include "anchor_manager.hpp"
#include "highlight_rule_set.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_session.hpp"
#include "log_session_view.hpp"
#include "log_table_view.hpp"
#include "row_order_proxy_model.hpp"

#include <QAbstractItemView>
#include <QItemSelectionModel>

SessionBindContext SessionBindContext::FromSessionAndView(
    LogSession *session, LogSessionView *view, ThemeControl *theme
)
{
    // A partial pair cannot safely bind session-owned UI.
    if (session == nullptr || view == nullptr)
    {
        return SessionBindContext::MakeUnbound();
    }

    SessionBindContext context;
    context.theme = theme;

    context.session = session;
    context.model = session->Model();
    context.rowOrderProxy = session->RowOrderProxy();
    context.filterProxy = session->FilterProxy();
    context.anchors = session->Anchors();
    context.highlights = session->Highlights();

    context.view = view;
    // The table may already be unavailable during teardown.
    if (auto *table = view->TableView(); table != nullptr)
    {
        context.selection = table->selectionModel();
    }

    return context;
}
