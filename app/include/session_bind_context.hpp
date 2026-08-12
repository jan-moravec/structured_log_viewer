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
/// (task 5.1).
///
/// The concrete pointers are populated in Phase 2 (LogSession) and
/// Phase 3 (LogSessionView). Phase 5 wires each dock's Bind/Unbind
/// contract to consume this context.
struct SessionBindContext
{
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
    [[nodiscard]] bool IsBound() const noexcept
    {
        return !session.isNull() && !view.isNull() && !model.isNull() && !anchors.isNull() && !highlights.isNull();
    }
};
