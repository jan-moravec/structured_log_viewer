#include "log_table_view.hpp"

LogTableView::LogTableView(QWidget *parent) : QTableView(parent)
{
    setAcceptDrops(true);
}
