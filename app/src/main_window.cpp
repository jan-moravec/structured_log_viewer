#include "main_window.hpp"
#include "./ui_main_window.h"

#include <QTableView>
#include <QTimer>
#include <QStandardItemModel>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QMessageBox>

#include <chrono>
#include <filesystem>
#include <sstream>

#include <date/date.h>
#include <date/tz.h>

#include <json_parser.hpp>
#include <log_time.hpp>

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

    mTableView = new LogTableView(this);

    setCentralWidget(mTableView);

    //QVBoxLayout *layout = new QVBoxLayout(ui->centralWidget);

    //layout->addWidget(tableView);

    // Create the model
    mModel = new LogModel(mTableView);

    mTableView->horizontalHeader()->setStyleSheet(
        "QHeaderView::section {"
        "background-color: lightgray;"
        //"color: blue;"
        "padding: 5px;"
        "border: 1px solid #6c6c6c;"
        "font-weight: bold;"
        //"font-size: 12pt;"
        "}"
        );


    // Create the view
    mTableView->setModel(mModel);

    // Set selection behavior
    mTableView->setSelectionBehavior(QAbstractItemView::SelectRows);

    // Set selection mode to allow multiple selection
    mTableView->setSelectionMode(QAbstractItemView::MultiSelection);

    // Disable editing of individual cells
    mTableView->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // Set alternating row colors
    mTableView->setAlternatingRowColors(true);

    // TODO: Implement QSortFilterProxyModel
    // Enable sorting
    mSortFilterProxyModel = new QSortFilterProxyModel(this);
    mSortFilterProxyModel->setSourceModel(mModel);
    mSortFilterProxyModel->setSortRole(SortRole);
    mTableView->setModel(mSortFilterProxyModel);
    mTableView->setSortingEnabled(true);

    // Resize columns to fit contents
    mTableView->resizeColumnsToContents();

    // Set header customization
    mTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    mTableView->horizontalHeader()->resizeSections(QHeaderView::Stretch);
    mTableView->horizontalHeader()->setStretchLastSection(true);

    mTableView->horizontalHeader()->setHighlightSections(false);  // No highlight on header click
    mTableView->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);  // Align headers


    // Set cell alignment
    /*for (int row = 0; row < model->rowCount(); ++row) {
        for (int column = 0; column < model->columnCount(); ++column) {
            model->item(row, column)->setTextAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        }
    }*/

    // Adjust row height
    mTableView->verticalHeader()->setDefaultSectionSize(25);

    // Enable grid lines
    mTableView->setShowGrid(true);

    connect(ui->actionOpen, &QAction::triggered, this, &MainWindow::OpenFile);
    connect(ui->actionSaveConfiguration, &QAction::triggered, this, &MainWindow::SaveConfiguration);
    connect(ui->actionLoadConfiguration, &QAction::triggered, this, &MainWindow::LoadConfiguration);
    connect(ui->actionExit, &QAction::triggered, this, &MainWindow::close);

    QTimer::singleShot(0, [=] {
        mTableView->resizeColumnsToContents();
        //tableView->horizontalHeader()->stretchLastSection();  // Stretch to fill the view
        //tableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        //tableView->resizeRowsToContents();
    });
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::OpenFile()
{
    QStringList files = QFileDialog::getOpenFileNames(this,
                                                      "Select Files",
                                                      QString(),
                                                      "All Files (*.*)");

    if (!files.isEmpty()) {
        for (const QString &file : files) {
            OpenFileInternal(file);
        }
    }
}

void MainWindow::SaveConfiguration()
{
    QString file = QFileDialog::getSaveFileName(this,
                                                      "Save Configuration",
                                                      QString(),
                                                      "JSON (*.json);;All Files (*)");
    SerializeConfiguration(file.toStdString(), mConfiguration);
}

void MainWindow::LoadConfiguration()
{
    QString file = QFileDialog::getOpenFileName(this,
                                                "Save Configuration",
                                                QString(),
                                                "JSON (*.json);;All Files (*)");
    mConfiguration = DeserializeConfiguration(file.toStdString());
    mModel->AddData(LogData{}, mConfiguration);
}

void MainWindow::dropEvent(QDropEvent *event) {
    const QMimeData *mimeData = event->mimeData();

    if (mimeData->hasUrls()) {
        QList<QUrl> urlList = mimeData->urls();

        for (int i = 0; i < urlList.size(); ++i) {
            OpenFileInternal(urlList.at(i).toLocalFile());
        }

        event->acceptProposedAction();
    }
}

void MainWindow::OpenFileInternal(const QString &file)
{
    ParseResult result = JsonParser().Parse(file.toStdString());

    //LogConfiguration configuration;
    //configuration.columns[0] = LogConfiguration::Column{"Timestamp", {"timestamp"}, "%F %H:%M:%S", LogConfiguration::Type::Time, {"%FT%T%Ez"}};
    //configuration.columns[1] = LogConfiguration::Column{"Status", {"status"}, "{}"};
    //configuration.columns[2] = LogConfiguration::Column{"ID", {"id"}, "{}"};
    //configuration.columns[3] = LogConfiguration::Column{"Body", {"body"}, "{}"};

    UpdateConfiguration(mConfiguration, result.data);
    ParseTimestamps(result.data, mConfiguration);

    mModel->AddData(std::move(result.data), mConfiguration);
    if (!result.error.empty())
    {
        QMessageBox::warning(this, "Error parsing " + file, QString::fromStdString(result.error));
    }

    //mTableView->sortByColumn(0, Qt::SortOrder::AscendingOrder);
}
