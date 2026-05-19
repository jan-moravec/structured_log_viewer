#pragma once

#include <QDockWidget>
#include <QPointer>

class LogModel;
class RecordDetailWidget;

/// Dockable host for a single `RecordDetailWidget`. Owned by
/// `MainWindow`; lives next to the central table view.
///
/// The dock follows the table's currently-selected row when visible:
/// `MainWindow` calls `ShowSourceRow(int)` on selection changes and
/// `Clear()` on `modelReset`. The widget always reads through
/// `BuildRecordDetailContent` from the live `LogModel`, so streaming
/// FIFO eviction is reflected the next time the selection updates.
class RecordDetailDock : public QDockWidget
{
    Q_OBJECT

public:
    /// `model` is borrowed; must outlive the dock. `parent` becomes
    /// the `QDockWidget`'s parent (typically `MainWindow`).
    RecordDetailDock(LogModel *model, QWidget *parent = nullptr);

    /// Pin the dock to @p sourceRow of the source model and refresh
    /// the content. Negative or out-of-range rows clear the view.
    void ShowSourceRow(int sourceRow);

    /// Reset to the "no row" placeholder. Called from
    /// `LogModel::modelReset`.
    void Clear();

    /// Current source row, or -1 if the dock holds a placeholder.
    [[nodiscard]] int CurrentSourceRow() const noexcept
    {
        return mCurrentSourceRow;
    }

    [[nodiscard]] RecordDetailWidget *Widget() const noexcept
    {
        return mWidget;
    }

signals:
    /// User clicked the widget's "Open in new window" button. The
    /// argument is the currently-shown source row, or -1 when the
    /// dock holds a placeholder (in which case `MainWindow` should
    /// ignore the request).
    void openInNewWindowRequested(int sourceRow);

private:
    void RefreshFromModel();
    void OnOpenInNewWindowRequested();

    QPointer<LogModel> mModel;
    RecordDetailWidget *mWidget = nullptr;
    /// Source-model row currently being displayed; -1 means
    /// placeholder. Owners are expected to push every selection
    /// change through `ShowSourceRow`, so this stays in sync without
    /// the dock subscribing to model signals directly.
    int mCurrentSourceRow = -1;
};
