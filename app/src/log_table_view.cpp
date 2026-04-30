#include "log_table_view.hpp"

#include <log_model.hpp>

#include <QApplication>
#include <QClipboard>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QScrollBar>

LogTableView::LogTableView(QWidget *parent) : QTableView(parent)
{
    setAcceptDrops(true);

    // Edge-triggered emission of `userScrolledAwayFromBottom` /
    // `userScrolledToBottom` so the `MainWindow` only flips the
    // **Follow tail** toggle on transition (PRD 4.3.3).
    connect(verticalScrollBar(), &QAbstractSlider::valueChanged, this, [this](int value) {
        const bool atBottom = (value >= verticalScrollBar()->maximum());
        if (atBottom == mAtBottom)
        {
            return;
        }
        mAtBottom = atBottom;
        if (atBottom)
        {
            emit userScrolledToBottom();
        }
        else
        {
            emit userScrolledAwayFromBottom();
        }
    });
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
    const QModelIndexList selectedRows = this->selectionModel()->selectedRows();
    if (selectedRows.isEmpty())
    {
        return;
    }

    QString text;
    for (const QModelIndex &rowIndex : selectedRows)
    {
        const QVariant modelData = this->model()->data(rowIndex, LogModelItemDataRole::CopyLine);
        text += modelData.toString() + QLatin1Char('\n');
    }
    text.chop(1); // Drop the trailing newline

    QClipboard *clipboard = QApplication::clipboard();
    clipboard->setText(text);
}
