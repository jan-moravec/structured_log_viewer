#pragma once

#include "log_model.hpp"

#include <QDialog>
#include <QPointer>
#include <QString>

class QLabel;
class QPushButton;
class QTableWidget;

/// Modeless dialog showing per-column type-health diagnostics from
/// `LogTable::ComputeColumnTypeHealth`. One row per column, with
/// total / present / matching / mismatched slot counts and the
/// configured type. Read-only; edits go through the column editor
/// and refresh here on the next `LogModel::columnHealthChanged`.
class ConfigurationDiagnosticsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ConfigurationDiagnosticsDialog(LogModel *model, QWidget *parent = nullptr);

    /// Repopulate from `LogModel::ColumnHealth`. Auto-invoked on
    /// `columnHealthChanged`.
    void Refresh();

    /// Count of columns whose configured type does not match the
    /// data. Shared between the status-bar summary and the dialog.
    [[nodiscard]] static int MismatchedColumnCount(const LogModel &model);

#ifdef LOGAPP_BUILD_TESTING
    /// Test-only direct accessor. `findChild<QTableWidget*>` is
    /// unreliable on the GitHub-hosted Linux runner with Qt 6.8 +
    /// offscreen QPA, so tests reach the table through this bypass.
    [[nodiscard]] QTableWidget *TableForTest() const noexcept
    {
        return mTable;
    }
#endif

signals:
    /// Emitted when the user double-clicks a row. `MainWindow` opens
    /// the column editor in response.
    void editColumnRequested(int columnIndex);

private:
    QPointer<LogModel> mModel;
    QLabel *mSummaryLabel = nullptr;
    QTableWidget *mTable = nullptr;
    QPushButton *mRefreshButton = nullptr;
    QPushButton *mCloseButton = nullptr;
};
