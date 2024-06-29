#include "main_window.hpp"
#include "./ui_main_window.h"

#include <QTableView>
#include <QTimer>
#include <QStandardItemModel>

#include "log_model.hpp"

#include <chrono>
#include <filesystem>
#include <sstream>

#include <date/date.h>
#include <date/tz.h>


MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);

    auto dataPath = std::filesystem::current_path() / std::filesystem::path("tzdata");
    date::set_install(dataPath.string());

    std::istringstream in{"2024-05-21T10:06:57.933+02:00"};
    std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds> tp;
    in >> date::parse("%FT%T%Ez", tp);

    // Create a zoned_time in local time zone from the parsed time_point
    // auto local_zone = std::chrono::current_zone(); // Get the current local time zone
    // std::chrono::zoned_time<std::chrono::microseconds> local_time{local_zone, tp};
    // std::string formatted_local_time_with_timezone = std::format("current time: {:%Y-%m-%d %H:%M:%S.%z}\n", local_time);
    
    // Convert the time_point to zoned_time with the local time zone
    auto tz = date::current_zone();
    date::zoned_time local_time{tz, tp};
    std::string formatted_local_time_with_timezone = date::format("%FT%T%Ez", local_time);


    // Example log data
    LogMessage log1;
    log1.timestamp = 3;
    log1.message = "ABC";
    log1.severity = "ERROR";
    LogMessage log2;
    log2.timestamp = 2;
    log2.message = "DEF";
    log2.severity = "INFO";
    LogMessage log3;
    log3.timestamp = 4;
    log3.message = "GHJ";
    log3.severity = "DEBUG";

    LogSet logs;
    logs.insert(log1);
    logs.insert(log2);
    logs.insert(log3);

    // Create the model
    LogModel *model = new LogModel(ui->tableView);

    ui->tableView->horizontalHeader()->setStyleSheet(
        "QHeaderView::section {"
        "background-color: lightgray;"
        //"color: blue;"
        "padding: 5px;"
        "border: 1px solid #6c6c6c;"
        "font-weight: bold;"
        //"font-size: 12pt;"
        "}"
        );

    // Set header labels
    QStringList headerLabels;
    headerLabels << "Timestamp" << "Severity" << "Message";

    model->setLogData(logs, headerLabels);

    // Create the view
    ui->tableView->setModel(model);

    // Set selection behavior
    ui->tableView->setSelectionBehavior(QAbstractItemView::SelectRows);

    // Set alternating row colors
    ui->tableView->setAlternatingRowColors(true);

    // TODO: Implement QSortFilterProxyModel
    // Enable sorting
    //ui->tableView->setSortingEnabled(true);

    // Resize columns to fit contents
    ui->tableView->resizeColumnsToContents();

    // Set header customization
    ui->tableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    ui->tableView->horizontalHeader()->stretchLastSection();  // Stretch to fill the view
    ui->tableView->horizontalHeader()->setHighlightSections(false);  // No highlight on header click
    ui->tableView->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);  // Align headers

    // Set cell alignment
    /*for (int row = 0; row < model->rowCount(); ++row) {
        for (int column = 0; column < model->columnCount(); ++column) {
            model->item(row, column)->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }*/

    // Adjust row height
    ui->tableView->verticalHeader()->setDefaultSectionSize(25);

    // Enable grid lines
    ui->tableView->setShowGrid(true);

    QTimer::singleShot(0, [header = ui->tableView->horizontalHeader()] {
        header->setSectionResizeMode(QHeaderView::Interactive);
    });
}

MainWindow::~MainWindow()
{
    delete ui;
}
