#pragma once

#include <QDockWidget>

class FindRecordWidget;

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
    void RevealAndFocus();

private:
    FindRecordWidget *mWidget = nullptr;
};
