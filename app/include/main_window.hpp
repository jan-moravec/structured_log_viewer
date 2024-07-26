#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include "log_model.hpp"
#include "log_table_view.hpp"
#include "find_record_widget.hpp"
#include "log_filter_model.hpp"
#include "log_configuration.hpp"

#include <QMainWindow>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>
#include <QVBoxLayout>
#include <QPushButton>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (event->mimeData()->hasUrls()) {
            event->acceptProposedAction();
        }
    }

    void dragMoveEvent(QDragMoveEvent* event) override
    {
        if (event->mimeData()->hasUrls()) {
            event->acceptProposedAction();
        }
    }

    void dropEvent(QDropEvent *event) override;

    void UpdateUi()
    {
        for (int i = 0; i < mTableView->model()->columnCount() - 1; ++i) {
            mTableView->resizeColumnToContents(i);
        }
    }

private slots:
    void OpenFiles();
    void OpenJsonLogs();
    void SaveConfiguration();
    void LoadConfiguration();

    void Find();
    void FindRecords(const QString &text, bool next, bool wildcards, bool regularExpressions);
    void FindRecordsWidgedClosed();

private:
    void OpenFileInternal(const QString &file);

    Ui::MainWindow *ui;
    QVBoxLayout *mLayout;
    LogFilterModel *mSortFilterProxyModel;
    LogTableView *mTableView;
    LogModel *mModel;
    FindRecordWidget *mFindRecord = nullptr;
    loglib::LogConfiguration mConfiguration;
};
#endif // MAIN_WINDOW_H
