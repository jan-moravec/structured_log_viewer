#pragma once

#include <QPointer>
#include <QWidget>

class LogSession;

/// Per-tab visual workspace bound to exactly one `LogSession`.
///
/// Phase 1 declares the widget as a compile-only skeleton (task 1.8);
/// concrete contents (table, overview rail, delegates, selection,
/// scroll, follow-newest, jump-to-tail, focus restoration, view
/// context menus, tab-scoped progress presentation) arrive in
/// Phase 3.
///
/// Ownership contract (PRD §4.2 and task 3.1):
///
/// - Constructed with a live `LogSession` pointer and never
///   transplanted to another session for its lifetime.
/// - Must not be implemented as a nested `QMainWindow`; the tab
///   container hosts plain widgets so shell chrome (menus, docks,
///   toolbars, status bar) stays single-instance per window.
class LogSessionView : public QWidget
{
    Q_OBJECT

public:
    explicit LogSessionView(LogSession *session, QWidget *parent = nullptr);
    ~LogSessionView() override;

    LogSessionView(const LogSessionView &) = delete;
    LogSessionView &operator=(const LogSessionView &) = delete;
    LogSessionView(LogSessionView &&) = delete;
    LogSessionView &operator=(LogSessionView &&) = delete;

    /// The session this view is bound to for its whole lifetime.
    /// May return null after the session has been destroyed under
    /// teardown; docks and dialogs check before dereferencing.
    [[nodiscard]] LogSession *Session() const noexcept
    {
        return mSession.data();
    }

private:
    QPointer<LogSession> mSession;
};
