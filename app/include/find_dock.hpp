#pragma once

#include <QDockWidget>
#include <QPointer>

class FindRecordWidget;
class QCloseEvent;
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

protected:
    /// Restore focus to whatever held it before the bar opened.
    /// Wired to `hideEvent` rather than `closeEvent` so every
    /// dismissal path covers it: the dock's "X" button, Escape
    /// (via `DismissBar`), the View menu toggle (which calls
    /// `hide()`, bypassing `closeEvent`), and `restoreState`.
    /// `QPointer` covers the case where the prior focus widget
    /// was deleted while the bar was open (e.g. a config reload
    /// tore down the table view).
    void hideEvent(QHideEvent *event) override;

    /// Emit `closed` after the base class accepts the event so
    /// the menu toggle can drop the checkmark even when the dock
    /// is tabified (`visibilityChanged(false)` alone fires on tab
    /// inactivation, which would otherwise un-check the menu
    /// entry every time the user switches tabs).
    void closeEvent(QCloseEvent *event) override;

private:
    FindRecordWidget *mWidget = nullptr;

    /// Stashed focus target captured by `RevealAndFocus`. Cleared
    /// on `hideEvent` so a second hide can't re-steal focus.
    QPointer<QWidget> mFocusBeforeReveal;
};
