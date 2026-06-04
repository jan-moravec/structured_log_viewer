#pragma once

#include <QDockWidget>
#include <QString>

#include <string>
#include <vector>

class QListWidget;
class QPushButton;
class QLabel;

/// Dockable panel that shows accumulated parse / open errors.
///
/// Replaces the modal `QMessageBox::warning` that used to surface
/// errors from `MainWindow::ShowParseErrors`. A persistent panel
/// is better suited to streaming sessions where new errors can
/// arrive at any moment without the user wanting to dismiss a
/// dialog each time.
///
/// Position persists across runs via `QMainWindow::saveState()`
/// / `restoreState()` keyed on the dock's `objectName`. Entries
/// are session-scoped — a new file open or model reset calls
/// `Clear()`.
class ParseErrorsDock : public QDockWidget
{
    Q_OBJECT

public:
    explicit ParseErrorsDock(QWidget *parent = nullptr);

    /// Append one batch of errors under @p title. No-op for an
    /// empty @p errors. Auto-shows + raises the dock so the user
    /// notices the new entries.
    void AppendErrors(const QString &title, const std::vector<std::string> &errors);

    /// Drop every entry. Used when the user clicks the Clear
    /// button and from `MainWindow` on session discards.
    void ClearErrors();

    /// Total number of error entries currently displayed.
    [[nodiscard]] int Count() const noexcept;

signals:
    /// Emitted whenever `Count()` changes. The status-bar
    /// indicator listens here so it can hide itself when the
    /// dock empties.
    void countChanged(int count);

private:
    /// Refresh the header summary (e.g. "12 entries") and emit
    /// `countChanged`.
    void RefreshSummary();

    QListWidget *mList = nullptr;
    QLabel *mSummary = nullptr;
    QPushButton *mClearButton = nullptr;
};
