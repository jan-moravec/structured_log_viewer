#pragma once

#include <QDialog>
#include <QPointer>

class LogModel;
class MainWindow;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;

/// Modeless dialog that lets the user manage every column at once:
/// reorder rows (Move up / Move down), toggle visibility in-place,
/// or drill into the per-column `ColumnEditor` for the full type /
/// autoDetect / header edit. Edits land immediately through the
/// model -- the manager has a single **Close** button rather than an
/// OK/Cancel split, matching the in-place feel of header drag-reorder
/// and the View menu's per-column toggles.
///
/// Wired to `LogModel::headerDataChanged`, `LogModel::modelReset`,
/// and `LogModel::columnHealthChanged` so the table re-renders when
/// anything outside the manager (column reorder via header drag,
/// streaming-driven type promotion, configuration load) shifts the
/// underlying columns.
class ColumnsManagerDialog : public QDialog
{
    Q_OBJECT

public:
    /// Construct a manager bound to @p model. The parent should be
    /// the owning `MainWindow` so the dialog can delegate the
    /// per-column drill-down through `MainWindow::EditColumn` (which
    /// repaints visibility + status bar after the editor commits).
    /// `mainWindow` can be `nullptr` in tests; in that case the
    /// Edit... button writes through `ColumnEditor` directly.
    ColumnsManagerDialog(LogModel *model, MainWindow *mainWindow, QWidget *parent = nullptr);

    /// Repopulate every row from the model. Auto-invoked when the
    /// model signals shape / header changes; tests call it directly
    /// to assert the contents.
    void Refresh();

    /// Move the column at @p row up one slot via `LogModel::MoveColumn`.
    /// Public so the test seam can drive the path without resolving
    /// `QPushButton` children. No-op when the row is already the
    /// first one.
    void MoveSelectedUp();

    /// Symmetric to `MoveSelectedUp`. No-op when the row is already
    /// the last one.
    void MoveSelectedDown();

    /// Open the `ColumnEditor` for the currently selected row.
    /// Tests can drive this without poking at the button. No-op when
    /// nothing is selected.
    void EditSelected();

private slots:
    /// Quick visibility toggle in the table itself. The checkbox
    /// state is the only mutable widget on a row; everything else is
    /// read-only and routes through `EditSelected`.
    void OnItemChanged(QTableWidgetItem *item);

private:
    void RebuildRow(int row);
    [[nodiscard]] int CurrentRow() const;

    QPointer<LogModel> mModel;
    QPointer<MainWindow> mMainWindow;
    QTableWidget *mTable = nullptr;
    QPushButton *mMoveUpButton = nullptr;
    QPushButton *mMoveDownButton = nullptr;
    QPushButton *mEditButton = nullptr;
    QPushButton *mCloseButton = nullptr;

    /// Re-entrancy guard for `OnItemChanged`. `Refresh` and
    /// `RebuildRow` programmatically set check states; without the
    /// guard, every refresh would echo back through the slot and
    /// re-invoke `SetColumnVisible` on every cell.
    bool mUpdatingProgrammatically = false;
};
