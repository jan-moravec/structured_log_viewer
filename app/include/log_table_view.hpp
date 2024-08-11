#pragma once

#include "log_model.hpp"

#include <QTableView>

class LogTableView : public QTableView
{
public:
    explicit LogTableView(QWidget *parent = nullptr);

    void keyPressEvent(QKeyEvent *event) override;

public slots:
    void CopySelectedRowsToClipboard();
};
