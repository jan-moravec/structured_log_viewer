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

// -----------------------------------------------------------------------
// Task 5.1: single-point builder for the active-session bind context.
//
// The builder reads every session-owned pointer via the session's / view's
// public accessors so a future addition to the struct requires updating
// exactly one factory (plus the field declaration + the `IsBound()` gate).
// Docks and dialogs consume the resulting context via their
// `Bind(SessionBindContext)` slots.
//
// Contract (matches the header docstring): the returned context is either
// (a) fully bound -- every session-owned pointer non-null -- or (b) fully
// unbound -- equivalent to `SessionBindContext::MakeUnbound()`. Callers do
// NOT need to guard `activeSession()` / `activeSessionView()` before
// calling in; a partial pair (session with no view, or view with no
// session) collapses to `MakeUnbound()` so downstream Bind slots see one
// of the two shapes, never a half-populated context.
//
// Rationale: phase 6 promises each dock's `Bind` an all-or-nothing
// contract. A partial context (say model non-null but selection null)
// would silently retarget the dock at a session with no visible view,
// and any dock that reads `context.selection` unguarded would deref a
// null QPointer. Origin-review finding M6: docks never call `IsBound()`
// before consuming individual pointers; making the factory itself
// enforce the invariant closes the hole at the source rather than
// requiring every dock to add the check.
// -----------------------------------------------------------------------
SessionBindContext SessionBindContext::FromSessionAndView(
    LogSession *session, LogSessionView *view, ThemeControl *theme
)
{
    // Enforce the all-or-nothing contract: a partial pair collapses to
    // MakeUnbound so downstream Bind slots see one of the two shapes.
    // `theme` is intentionally dropped in the unbound case -- it is
    // window-scoped, not session-scoped, and the Bind slots that read
    // it also read a session-owned pointer that would be null, so
    // leaking the theme alone accomplishes nothing.
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
    // The view's selection model is only available after the view has
    // installed its table's `setModel(...)`. In the phase-3 ctor path
    // that ordering is guaranteed (see `LogSessionView::Initialise()`);
    // guard defensively so an in-flight teardown that has already
    // dropped the table still returns a safe null selection.
    if (auto *table = view->TableView(); table != nullptr)
    {
        context.selection = table->selectionModel();
    }

    return context;
}
