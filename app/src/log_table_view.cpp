#include "log_table_view.hpp"

#include <log_model.hpp>

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
        const QVariant modelData = this->model()->data(rowIndex, LogModelItemDataRole::CopyLine);
        text += modelData.toString() + "\n";
    }
    text.removeLast();
    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(text);
}
