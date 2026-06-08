#pragma once

#include <QDockWidget>
#include <QPointer>

class FindRecordWidget;
class QCloseEvent;
class QShowEvent;
class QWidget;

/// Dockable host for `FindRecordWidget`. Position persists via
/// `QMainWindow::saveState()` / `restoreState()`.
///
/// Allowed areas are top + bottom only; a vertical side dock would
/// squeeze the search field into an unusable narrow column.
class FindDock : public QDockWidget
{
    Q_OBJECT

public:
    explicit FindDock(QWidget *parent = nullptr);

    /// The hosted find widget. Borrow only; the dock owns it.
    [[nodiscard]] FindRecordWidget *Widget() const noexcept
    {
        return mWidget;
    }

    /// Show + raise the dock and focus the search field. Idempotent.
    /// On every call, stashes the previously-focused widget (when
    /// outside our subtree) so dismissing the bar can restore it.
    void RevealAndFocus();

signals:
    /// Emitted on genuine user dismissal (X button, `close()` from
    /// `DismissBar`, system close). Distinct from
    /// `visibilityChanged(false)`, which also fires on tab inactivation.
    void closed();

    /// Emitted when the bar becomes visible (cold reveal or tab
    /// activation). Named alias for `visibilityChanged(true)` that
    /// keeps the wiring self-documenting.
    void revealed();

protected:
    /// Restore focus to the stashed widget, then emit `closed`.
    void closeEvent(QCloseEvent *event) override;

    /// Emit `revealed` so listeners can refresh state that went stale
    /// while the bar was hidden / tab-buried.
    void showEvent(QShowEvent *event) override;

private:
    FindRecordWidget *mWidget = nullptr;

    /// Widget that held focus before the last reveal. `QPointer`
    /// guards against the widget being destroyed while the bar is open.
    QPointer<QWidget> mFocusBeforeReveal;
};
