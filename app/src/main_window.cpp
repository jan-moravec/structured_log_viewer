#include "main_window.hpp"
#include "./ui_main_window.h"

#include <QTableView>
#include <QTimer>
#include <QStandardItemModel>
#include <QHeaderView>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QLineEdit>
#include <QCheckBox>

#include <filesystem>

#include <date/date.h>
#include <date/tz.h>

#include <log_factory.hpp>
#include <log_time.hpp>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    this->setWindowTitle("Structured Log Viewer");

    mTableView = new LogTableView(this);

    //setCentralWidget(mTableView);

    mLayout = new QVBoxLayout(ui->centralWidget);
    //hLayout->setContentsMargins(0, 0, 0, 0);
    //hLayout->setSpacing(0);

    mLayout->addWidget(mTableView, 1);
    mTableView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);



    //customWindow->show();

    //customWindow->close();

    //QVBoxLayout *layout = new QVBoxLayout(ui->centralWidget);

    //layout->addWidget(tableView);

    // Create the model
    mModel = new LogModel(mTableView);

    //mTableView->setStyleSheet("QTableView::item:selected { background-color: lightblue; }");

//    QTableView::item:focus { background-color: lightblue; }
//    QTableView::item:focus:!active { background-color: lightblue; }
    // QTableView::item:selected:focus { outline: none; border: none; }
    // QTableView::item {outline: none; border: none; background: transparent; }
    // QTableView::item:focus:pressed { outline: none; border: none; background: transparent; }
    //  background: transparent;
    //QTableView::item:focus { outline: none; border: none; background: transparent; color: #000000; }

    mTableView->setStyleSheet(R"(


QTableView::item:selected { background-color: lightblue; }
QTableView::item:selected:!active { background-color: lightblue; }




)");

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
    mSortFilterProxyModel = new LogFilterModel(this);
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

    connect(ui->actionOpen, &QAction::triggered, this, &MainWindow::OpenFiles);
    connect(ui->actionOpenJsonLogs, &QAction::triggered, this, &MainWindow::OpenJsonLogs);
    connect(ui->actionSaveConfiguration, &QAction::triggered, this, &MainWindow::SaveConfiguration);
    connect(ui->actionLoadConfiguration, &QAction::triggered, this, &MainWindow::LoadConfiguration);
    connect(ui->actionExit, &QAction::triggered, this, &MainWindow::close);

    connect(ui->actionCopy, &QAction::triggered, mTableView, &LogTableView::CopySelectedRowsToClipboard);
    connect(ui->actionFind, &QAction::triggered,  this, &MainWindow::Find);

    QTimer::singleShot(0, [this] {
        try
        {
            loglib::Initialize();
        }
        catch (std::exception &e)
        {
            QMessageBox::critical(this, "Fatal Error", QString("An unrecoverable error occurred:\n") + e.what() + "\n\nApplication will exit.");
            QApplication::exit(1);
        }
        //mTableView->resizeColumnsToContents();
        //tableView->horizontalHeader()->stretchLastSection();  // Stretch to fill the view
        //tableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
        //tableView->resizeRowsToContents();
    });
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::OpenFiles()
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

void MainWindow::OpenJsonLogs()
{
    QStringList files = QFileDialog::getOpenFileNames(this,
                                                      "Select Files",
                                                      QString(),
                                                      "All Files (*.*)");

    std::unique_ptr<LogParser> jsonParser = LogFactory::Create(LogFactory::Parser::Json);
    for (const QString &file : files)
    {
        if (!files.isEmpty()) {

            ParseResult result = jsonParser->Parse(file.toStdString());
            UpdateConfiguration(mConfiguration, result.data);
            ParseTimestamps(result.data, mConfiguration);

            mModel->AddData(std::move(result.data), mConfiguration);
            if (!result.error.empty())
            {
                QMessageBox::warning(this, "Error Parsing JSON Logs", QString::fromStdString(result.error));
            }

            UpdateUi();
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
    if (!file.isEmpty())
    {
        try
        {
            mConfiguration = DeserializeConfiguration(file.toStdString());
            mModel->AddData(LogData{}, mConfiguration);
            UpdateUi();
        }
        catch (std::exception &e)
        {
            QMessageBox::warning(this, "Error Parsing Configuration", e.what());
        }
    }
}

void MainWindow::Find()
{
    if (mFindRecord == nullptr)
    {
        mFindRecord = new FindRecordWidget(this);
        connect(mFindRecord, &FindRecordWidget::FindRecords,  this, &MainWindow::FindRecords);
        connect(mFindRecord, &FindRecordWidget::Closed,  this, &MainWindow::FindRecordsWidgedClosed);
        mLayout->addWidget(mFindRecord);
        mFindRecord->setFocus();
    }
    else
    {
        mFindRecord->setFocus();
    }

    mFindRecord->SetEditFocus();
}

void MainWindow::FindRecords(const QString &text, bool next, bool wildcards, bool regularExpressions)
{
    QModelIndex searchStartIndex;
    if(!mTableView->currentIndex().isValid())
    {
        searchStartIndex = mTableView->model()->index(0,0);
    }
    else
    {
        searchStartIndex = mTableView->currentIndex();
    }
    Qt::MatchFlags flags = Qt::MatchContains | Qt::MatchWrap | Qt::MatchRecursive;
    if (wildcards)
    {
        flags |= Qt::MatchWildcard;
    }
    if (regularExpressions)
    {
        flags |= Qt::MatchRegularExpression;
    }
    int skipFirstN = 0;
    if (mTableView->selectionModel()->isRowSelected(searchStartIndex.row()))
    {
        skipFirstN = 1;
    }

    const QVariant value = QVariant::fromValue(text);
    QModelIndexList matches = mSortFilterProxyModel->MatchRow(
        mModel->index(searchStartIndex.row(), 0),
        Qt::DisplayRole,
        value,
        1,
        flags,
        next,
        skipFirstN
        );

    if (!matches.isEmpty())
    {
        mTableView->clearSelection();
        mTableView->scrollTo(matches[0]);
        mTableView->selectionModel()->select(matches[0], QItemSelectionModel::Select | QItemSelectionModel::Rows);
        mTableView->selectionModel()->setCurrentIndex(matches[0], QItemSelectionModel::NoUpdate);
    }
}

void MainWindow::FindRecordsWidgedClosed()
{
    mFindRecord = nullptr;
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
    bool isConfiguration = true;
    try
    {
        mConfiguration = DeserializeConfiguration(file.toStdString());
    } catch (...)
    {
        isConfiguration = false;
    }

    if (!isConfiguration)
    {
        ParseResult result = LogFactory::Parse(file.toStdString());

        UpdateConfiguration(mConfiguration, result.data);
        ParseTimestamps(result.data, mConfiguration);

        mModel->AddData(std::move(result.data), mConfiguration);
        if (!result.error.empty())
        {
            QMessageBox::warning(this, "Error Opening File", QString::fromStdString(result.error));
        }
    }

    UpdateUi();
    //mTableView->sortByColumn(0, Qt::SortOrder::AscendingOrder);
}
