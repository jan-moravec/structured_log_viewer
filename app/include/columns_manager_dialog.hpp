#pragma once

#include <QDialog>
#include <QPointer>

class LogModel;
class MainWindow;
class QLabel;
class QPushButton;
class QTableWidget;
class QTableWidgetItem;

/// Modeless dialog that lists every column with Move up / Move down,
/// an in-place Visible checkbox, and an Edit... button that opens the
/// per-column `ColumnEditor`. All edits land immediately through the
/// model, so the dialog has a single Close button instead of OK/Cancel.
///
/// Auto-refreshes on `LogModel::headerDataChanged`, `modelReset`, and
/// `columnHealthChanged` so out-of-band column changes (header drag,
/// streaming type promotion, configuration load) stay reflected.
class ColumnsManagerDialog : public QDialog
{
    Q_OBJECT

public:
    /// Bind to @p model. @p mainWindow lets the Edit... button route
    /// through `MainWindow::EditColumn` (which also repaints the
    /// header and status bar). `nullptr` is allowed for tests; the
    /// dialog then drives `ColumnEditor` directly.
    ColumnsManagerDialog(LogModel *model, MainWindow *mainWindow, QWidget *parent = nullptr);

    /// Repopulate every row from the model.
    void Refresh();

    /// Refresh rows in the inclusive `[firstColumn, lastColumn]`
    /// range. Falls back to a full `Refresh()` when the row count
    /// no longer matches the model (i.e. a shape change).
    void RefreshRange(int firstColumn, int lastColumn);

    /// Move the selected column up one slot. No-op at the top edge.
    void MoveSelectedUp();

    /// Move the selected column down one slot. No-op at the bottom edge.
    void MoveSelectedDown();

    /// Open the `ColumnEditor` for the selected row. No-op if no
    /// selection.
    void EditSelected();

    /// Re-stamp the intro label's muted foreground against the
    /// current palette. The constructor's explicit `setPalette`
    /// freezes the colour, so palette changes need this nudge.
    /// Idempotent.
    void RefreshPalette();

#ifdef LOGAPP_BUILD_TESTING
    /// Test-only direct accessor for the columns table widget.
    /// `findChild<QTableWidget*>("columnsTable")` is unreliable on
    /// the GitHub-hosted Linux runner with Qt 6.8 + offscreen QPA,
    /// so tests reach the table through this bypass.
    [[nodiscard]] QTableWidget *TableForTest() const noexcept
    {
        return mTable;
    }
#endif

private slots:
    /// Slot for the in-row visibility checkbox; everything else on
    /// the row is read-only.
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
    /// Intro text. Kept as a member so `RefreshPalette` can
    /// re-apply its muted foreground on theme change.
    QLabel *mIntroLabel = nullptr;

    /// Suppresses `OnItemChanged` while `Refresh` / `RebuildRow` are
    /// programmatically setting check states.
    bool mUpdatingProgrammatically = false;
};
