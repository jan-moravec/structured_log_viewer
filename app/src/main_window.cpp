#include "main_window.hpp"
#include "./ui_main_window.h"

#include <QCheckBox>
#include <QFileDialog>
#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>
#include <QStandardItemModel>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <log_factory.hpp>
#include <log_processing.hpp>

bool IsDarkTheme()
{
    QColor bgColor = qApp->palette().color(QPalette::Window);
    int brightness = (bgColor.red() * 299 + bgColor.green() * 587 + bgColor.blue() * 114) / 1000;
    return brightness < 128;
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    this->setWindowTitle("Structured Log Viewer");

    mTableView = new LogTableView(this);
    mLayout = new QVBoxLayout(ui->centralWidget);
    mLayout->addWidget(mTableView, 1);
    mTableView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    // Create the model
    mModel = new LogModel(mTableView);

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

    if (IsDarkTheme())
    {
        mTableView->setStyleSheet(R"(
QTableView { background-color: #222222; alternate-background-color: #333333; }
QTableView::item:selected { background-color: #00518F; }
QTableView::item:selected:!active { background-color: #00518F; }
)");
    }
    else
    {
        mTableView->setStyleSheet(R"(
QTableView { background-color: #FFFFFF; alternate-background-color: #F0F0F0; }
QTableView::item:selected { background-color: #ADD4FF; color: black; }
QTableView::item:selected:!active { background-color: #ADD4FF; color: black; }
)");
    }

    // Enable sorting
    mSortFilterProxyModel = new LogFilterModel(this);
    mSortFilterProxyModel->setSourceModel(mModel);
    mSortFilterProxyModel->setSortRole(SortRole);
    mTableView->setModel(mSortFilterProxyModel);
    mTableView->setSortingEnabled(true);

    // Resize columns to fit contents
    mTableView->resizeColumnsToContents();

    // Set header customization
    mTableView->horizontalHeader()->setStyleSheet(R"(QHeaderView::section { padding: 8px; font-weight: bold; })");
    mTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    mTableView->horizontalHeader()->resizeSections(QHeaderView::Stretch);
    mTableView->horizontalHeader()->setStretchLastSection(true);

    mTableView->horizontalHeader()->setHighlightSections(false); // No highlight on header click
    mTableView->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter); // Align headers

    // Enable grid lines
    mTableView->setShowGrid(true);

    connect(ui->actionOpen, &QAction::triggered, this, &MainWindow::OpenFiles);
    connect(ui->actionOpenJsonLogs, &QAction::triggered, this, &MainWindow::OpenJsonLogs);
    connect(ui->actionSaveConfiguration, &QAction::triggered, this, &MainWindow::SaveConfiguration);
    connect(ui->actionLoadConfiguration, &QAction::triggered, this, &MainWindow::LoadConfiguration);
    connect(ui->actionExit, &QAction::triggered, this, &MainWindow::close);

    connect(ui->actionCopy, &QAction::triggered, mTableView, &LogTableView::CopySelectedRowsToClipboard);
    connect(ui->actionFind, &QAction::triggered, this, &MainWindow::Find);

    mFindRecord = new FindRecordWidget(this);
    connect(mFindRecord, &FindRecordWidget::FindRecords, this, &MainWindow::FindRecords);
    mLayout->addWidget(mFindRecord);
    mFindRecord->hide();

    QTimer::singleShot(0, [this] {
        try
        {
            loglib::Initialize();
        }
        catch (std::exception &e)
        {
            QMessageBox::critical(
                this,
                "Fatal Error",
                QString("An unrecoverable error occurred:\n") + e.what() + "\n\nApplication will exit."
            );
            QApplication::exit(1);
        }
    });
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (event->mimeData()->hasUrls())
    {
        event->acceptProposedAction();
    }
}

void MainWindow::dragMoveEvent(QDragMoveEvent *event)
{
    if (event->mimeData()->hasUrls())
    {
        event->acceptProposedAction();
    }
}

void MainWindow::dropEvent(QDropEvent *event)
{
    const QMimeData *mimeData = event->mimeData();

    if (mimeData->hasUrls())
    {
        QList<QUrl> urlList = mimeData->urls();

        for (int i = 0; i < urlList.size(); ++i)
        {
            OpenFileInternal(urlList.at(i).toLocalFile());
        }

        event->acceptProposedAction();
    }
}

void MainWindow::UpdateUi()
{
    for (int i = 0; i < mTableView->model()->columnCount() - 1; ++i)
    {
        mTableView->resizeColumnToContents(i);
    }
}

void MainWindow::OpenFiles()
{
    QStringList files = QFileDialog::getOpenFileNames(this, "Select Files", QString(), "All Files (*.*)");

    if (!files.isEmpty())
    {
        for (const QString &file : files)
        {
            OpenFileInternal(file);
        }
    }
}

void MainWindow::OpenJsonLogs()
{
    QStringList files = QFileDialog::getOpenFileNames(this, "Select Files", QString(), "All Files (*.*)");

    std::unique_ptr<LogParser> jsonParser = LogFactory::Create(LogFactory::Parser::Json);
    for (const QString &file : files)
    {
        if (!files.isEmpty())
        {

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
    QString file = QFileDialog::getSaveFileName(this, "Save Configuration", QString(), "JSON (*.json);;All Files (*)");
    SerializeConfiguration(file.toStdString(), mConfiguration);
}

void MainWindow::LoadConfiguration()
{
    QString file = QFileDialog::getOpenFileName(this, "Save Configuration", QString(), "JSON (*.json);;All Files (*)");
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
    mFindRecord->show();
    mFindRecord->setFocus();
    mFindRecord->SetEditFocus();
}

void MainWindow::FindRecords(const QString &text, bool next, bool wildcards, bool regularExpressions)
{
    QModelIndex searchStartIndex;
    if (!mTableView->currentIndex().isValid())
    {
        searchStartIndex = mTableView->model()->index(0, 0);
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
        mModel->index(searchStartIndex.row(), 0), Qt::DisplayRole, value, 1, flags, next, skipFirstN
    );

    if (!matches.isEmpty())
    {
        mTableView->clearSelection();
        mTableView->scrollTo(matches[0]);
        mTableView->selectionModel()->select(matches[0], QItemSelectionModel::Select | QItemSelectionModel::Rows);
        mTableView->selectionModel()->setCurrentIndex(matches[0], QItemSelectionModel::NoUpdate);
    }
}

void MainWindow::OpenFileInternal(const QString &file)
{
    bool isConfiguration = true;
    try
    {
        mConfiguration = DeserializeConfiguration(file.toStdString());
    }
    catch (...)
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
}
