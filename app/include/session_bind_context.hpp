#pragma once

#include <QPointer>

class LogSession;
class LogSessionView;
class LogModel;
class LogFilterModel;
class RowOrderProxyModel;
class AnchorManager;
class HighlightRuleSet;
class ThemeControl;
class QItemSelectionModel;

/// Guarded, non-owning bundle of pointers a shared dock or dialog
/// needs to bind to the "active session context" for one window.
/// Every field is a `QPointer` (or an equivalent guarded handle) so
/// a destroyed session leaves the context safely null, and a
/// re-entrant `Bind` during teardown observes the same nulls.
///
/// A default-constructed context represents an explicit *empty*
/// binding: docks in that state must clear their persistent model
/// indexes, cancel debounce timers, and hide session-specific chrome
/// (task 5.1). `SessionBindContext::MakeUnbound()` is a named alias
/// for the default ctor that reads clearer at call sites; use it
/// when the intent is "this dock has no active session right now".
///
/// The concrete pointers are populated in Phase 2 (LogSession) and
/// Phase 3 (LogSessionView). Phase 5 wires each dock's Bind/Unbind
/// contract to consume this context.
struct SessionBindContext
{
    /// Build a context from a live session + view pair. Reads
    /// every session-owned pointer via the session's / view's
    /// public accessors so a future addition to the struct
    /// requires updating exactly this one factory (plus the
    /// declaration + the `IsBound()` gate).
    ///
    /// `session` and `view` are collapsed to `MakeUnbound()` when
    /// EITHER is null. All-or-nothing: a partial pair (session
    /// with no view, or view with no session) is treated as
    /// "no active session" so downstream Bind slots see one of the
    /// two shapes -- fully bound or fully unbound -- and never a
    /// half-populated context (origin-review finding M6). This
    /// keeps callers that pass `activeSession()` /
    /// `activeSessionView()` (both of which are safe to be null
    /// during teardown) simple: they do not need to null-check
    /// first.
    ///
    /// @param session  the origin session; its model quintet
    ///                 populates the guarded pointers.
    /// @param view     the paired view; its selection model
    ///                 populates the guarded pointer.
    /// @param theme    non-owning, window-scoped. Passed through
    ///                 verbatim.
    [[nodiscard]] static SessionBindContext FromSessionAndView(
        LogSession *session, LogSessionView *view, ThemeControl *theme = nullptr
    );

    /// Named alias for the default ctor. Signals "no active
    /// session" at the call site; docks passing through their
    /// `Bind(SessionBindContext)` slot must treat this as
    /// equivalent to a full unbind.
    [[nodiscard]] static SessionBindContext MakeUnbound() noexcept
    {
        return SessionBindContext{};
    }

    QPointer<LogSession> session;
    QPointer<LogSessionView> view;

    QPointer<LogModel> model;
    QPointer<RowOrderProxyModel> rowOrderProxy;
    QPointer<LogFilterModel> filterProxy;

    QPointer<AnchorManager> anchors;
    QPointer<HighlightRuleSet> highlights;

    QPointer<QItemSelectionModel> selection;

    /// Non-owning. Windows share one `ThemeControl`; docks read
    /// it for accessible palette contrast. May be null in test
    /// fixtures that skip the themed constructor overload.
    ThemeControl *theme = nullptr;

    /// True iff every session-owned pointer resolves to a live
    /// object. Docks use this as the single unbind gate.
    ///
    /// Concretely, the checked fields are:
    ///
    ///   * `session`         — the `LogSession` itself,
    ///   * `view`            — the paired `LogSessionView`,
    ///   * `model`           — the session-owned `LogModel`,
    ///   * `rowOrderProxy`   — the session-owned display-order proxy,
    ///   * `filterProxy`     — the session-owned filter/sort proxy,
    ///   * `anchors`         — the session-owned `AnchorManager`,
    ///   * `highlights`      — the session-owned `HighlightRuleSet`.
    ///
    /// Deliberately NOT part of the gate:
    ///
    ///   * `selection`       — view-owned (`QTableView::selectionModel()`);
    ///                         a bound context with no selection yet
    ///                         is a legitimate transient state during
    ///                         setup.
    ///   * `theme`           — window-scoped; may legitimately be
    ///                         null in test fixtures that skip the
    ///                         themed constructor.
    ///
    /// If a new session-owned pointer is added to this struct, add
    /// the corresponding `!field.isNull()` clause here AND extend
    /// `TestIsBoundGatesOnEverySessionOwnedField` in
    /// `dock_binding_test.cpp` so a future omission fails at CI
    /// time rather than in a dock body against a null pointer.
    [[nodiscard]] bool IsBound() const noexcept
    {
        return !session.isNull() && !view.isNull() && !model.isNull() && !rowOrderProxy.isNull() &&
               !filterProxy.isNull() && !anchors.isNull() && !highlights.isNull();
    }
};
