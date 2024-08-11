#include "log_table_view.hpp"

#include <QApplication>
#include <QClipboard>
#include <QItemSelectionModel>
#include <QKeyEvent>

LogTableView::LogTableView(QWidget *parent) : QTableView(parent)
{
    setAcceptDrops(true);
}

void LogTableView::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Copy))
    {
        CopySelectedRowsToClipboard();
        return;
    }
    QTableView::keyPressEvent(event);
}

void LogTableView::CopySelectedRowsToClipboard()
{
    QString text;
    for (const QModelIndex &rowIndex : this->selectionModel()->selectedRows())
    {
        QVariant data = this->model()->data(rowIndex, LogModelItemDataRole::CopyLine);
        text += data.toString() + "\n";
    }
    text.removeLast();
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(text);
}
