#pragma once

#include "find_record_widget.hpp"
#include "log_configuration.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_table_view.hpp"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMainWindow>
#include <QMimeData>
#include <QPushButton>
#include <QVBoxLayout>

QT_BEGIN_NAMESPACE
namespace Ui
{
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    MainWindow(QWidget *parent = nullptr);
    ~MainWindow();

    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    void UpdateUi();

private slots:
    void OpenFiles();
    void OpenJsonLogs();
    void SaveConfiguration();
    void LoadConfiguration();

    void Find();
    void FindRecords(const QString &text, bool next, bool wildcards, bool regularExpressions);

    void AddFilter();
    void FilterSubmitted(const QString &filterID, int row, const QString &filterString, Qt::MatchFlags matchType);

private:
    void OpenFileInternal(const QString &file);

    Ui::MainWindow *ui;
    QVBoxLayout *mLayout;
    LogFilterModel *mSortFilterProxyModel;
    LogTableView *mTableView;
    LogModel *mModel;
    FindRecordWidget *mFindRecord;
    loglib::LogConfiguration mConfiguration;
};
