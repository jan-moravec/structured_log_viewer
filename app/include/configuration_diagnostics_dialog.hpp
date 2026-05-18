#pragma once

#include "log_model.hpp"

#include <QDialog>
#include <QPointer>
#include <QString>

class QLabel;
class QPushButton;
class QTableWidget;

/// Modeless dialog that surfaces per-column type-health diagnostics
/// computed by `LogTable::ComputeColumnTypeHealth`. Each row maps to
/// one source-table column and reports total / present / matching /
/// mismatched slot counts plus the configured type. The dialog is a
/// passive viewer; column-type edits happen through the Column
/// Editor (separate dialog) and are reflected here on the next
/// `LogModel::columnHealthChanged` emission.
class ConfigurationDiagnosticsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ConfigurationDiagnosticsDialog(LogModel *model, QWidget *parent = nullptr);

    /// Repopulate the table from the model's current configuration +
    /// `LogModel::ColumnHealth` snapshot. Auto-invoked when the model
    /// emits `columnHealthChanged`.
    void Refresh();

    /// Number of columns whose configured type does not match the
    /// data, aggregated from `LogModel::ColumnHealth`. Public so
    /// `MainWindow` and tests can derive the status-bar summary text
    /// without duplicating the aggregation.
    [[nodiscard]] static int MismatchedColumnCount(const LogModel &model);

private:
    QPointer<LogModel> mModel;
    QLabel *mSummaryLabel = nullptr;
    QTableWidget *mTable = nullptr;
    QPushButton *mRefreshButton = nullptr;
    QPushButton *mCloseButton = nullptr;
};
