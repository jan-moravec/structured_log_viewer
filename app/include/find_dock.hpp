#pragma once

#include <QDockWidget>
#include <QPointer>

class FindRecordWidget;
class QCloseEvent;
class QShowEvent;
class QWidget;

/// Dockable host for `FindRecordWidget`.
///
/// Promotes the find bar to first-class window furniture matching
/// `RecordDetailDock` and the anchors dock: free floating /
/// dockable / closable chrome, with position persisted via
/// `QMainWindow::saveState()` / `restoreState()` (keyed on the
/// dock's `objectName`).
///
/// Allowed areas are top + bottom only — find bars are horizontal
/// strips by convention; a vertical side dock would force the
/// search field into an unnatural narrow column.
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

    /// Show + raise the dock and focus the embedded line edit so
    /// the next keystroke lands in the search field. Idempotent.
    ///
    /// Captures the currently-focused widget so a subsequent
    /// `hideEvent` (Esc, "X" button, View menu toggle off) can
    /// hand focus back to it. Without this, dismissing the bar
    /// leaves focus dangling on whatever Qt picks next.
    void RevealAndFocus();

signals:
    /// Emitted whenever the user actually closes the dock (X
    /// button, `close()` from `DismissBar`, or system close). Not
    /// emitted when the dock is merely hidden as the inactive tab
    /// of a tabified group -- `visibilityChanged(false)` fires on
    /// tab switches too, so the menu toggle uses this signal to
    /// distinguish "user dismissed me" from "I'm just buried".
    void closed();

    /// Emitted when the dock becomes visible to the user, whether
    /// from a cold reveal or a tab activation. Mirrors
    /// `QDockWidget::visibilityChanged(true)` but is documented as
    /// the canonical "the user is looking at the find bar now"
    /// hook so `MainWindow` can refresh the match count after
    /// model activity that happened while the bar was buried.
    void revealed();

protected:
    /// Restore focus to whatever held it before the bar opened,
    /// then emit `closed`. Both live here (rather than in
    /// `hideEvent`) so tab inactivation in a tabified group
    /// does not steal focus from the newly-active sibling tab:
    /// `hideEvent` fires on every tab switch, `closeEvent` only on
    /// genuine dismissal (X button, Escape via `DismissBar`, View
    /// menu toggle off -- all of which route through `close()`).
    ///
    /// `QPointer` on `mFocusBeforeReveal` covers the case where
    /// the prior focus widget was deleted while the bar was open
    /// (e.g. a config reload tore down the table view).
    void closeEvent(QCloseEvent *event) override;

    /// Emit `revealed` so `MainWindow` can refresh the match
    /// count if model / proxy activity invalidated it while the
    /// bar was hidden or tab-buried. Connecting to plain
    /// `QDockWidget::visibilityChanged(true)` would work too, but
    /// keeping a named signal keeps the wiring self-documenting
    /// at the call site.
    void showEvent(QShowEvent *event) override;

private:
    FindRecordWidget *mWidget = nullptr;

    /// Stashed focus target captured by `RevealAndFocus`. Cleared
    /// when consumed by `closeEvent` so a second close (after a
    /// second reveal without a fresh focus target) can't restore
    /// to a stale pointer.
    QPointer<QWidget> mFocusBeforeReveal;
};
