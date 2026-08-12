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
