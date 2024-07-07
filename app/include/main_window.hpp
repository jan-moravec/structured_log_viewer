#ifndef MAIN_WINDOW_H
#define MAIN_WINDOW_H

#include "log_model.hpp"
#include "log_table_view.hpp"

#include <log_configuration.hpp>

#include <QMainWindow>
#include <QSortFilterProxyModel>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QMimeData>

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

private slots:
    void OpenFile();

private:
    void OpenFileInternal(const QString &file);

    Ui::MainWindow *ui;
    QSortFilterProxyModel *mSortFilterProxyModel;
    LogTableView *mTableView;
    LogModel *mModel;
    loglib::LogConfiguration mConfiguration;
};
#endif // MAIN_WINDOW_H
