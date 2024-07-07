#pragma once

#include "log_model.hpp"

#include <QTableView>
#include <QItemSelectionModel>
#include <QKeyEvent>
#include <QClipboard>
#include <QApplication>

class LogTableView : public QTableView
{
public:
    explicit LogTableView(QWidget *parent = nullptr);

    void keyPressEvent(QKeyEvent *event) override {
        if (event->matches(QKeySequence::Copy)) {
            QString text;
            for (const QModelIndex &rowIndex : this->selectionModel()->selectedRows()) {
                QVariant data = this->model()->data(rowIndex, LogModelItemDataRole::CopyLine);
                text += data.toString() + "\n";
            }
            text.removeLast();
            QClipboard *clipboard = QApplication::clipboard();
            clipboard->setText(text);
            return;
        }
        QTableView::keyPressEvent(event);
    }
};
