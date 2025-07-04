#include "main_window.hpp"
#include "./ui_main_window.h"

#include "appearance_control.hpp"
#include "filter_editor.hpp"

#include <loglib/log_factory.hpp>
#include <loglib/log_processing.hpp>

#include <QCheckBox>
#include <QDebug>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QStandardItemModel>
#include <QTableView>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent), ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    this->setWindowTitle("Structured Log Viewer");
    this->setWindowIcon(QIcon(":/icon-white.png"));

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

    if (AppearanceControl::IsDarkTheme())
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
    mTableView->sortByColumn(-1, Qt::SortOrder::AscendingOrder); // Do not sort automatically

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

    // Set smooth scrolling (scroll per pixel)
    mTableView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    mTableView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    connect(ui->actionOpen, &QAction::triggered, this, &MainWindow::OpenFiles);
    connect(ui->actionOpenJsonLogs, &QAction::triggered, this, &MainWindow::OpenJsonLogs);
    connect(ui->actionSaveConfiguration, &QAction::triggered, this, &MainWindow::SaveConfiguration);
    connect(ui->actionLoadConfiguration, &QAction::triggered, this, &MainWindow::LoadConfiguration);
    connect(ui->actionExit, &QAction::triggered, this, &MainWindow::close);

    connect(ui->actionCopy, &QAction::triggered, mTableView, &LogTableView::CopySelectedRowsToClipboard);
    connect(ui->actionFind, &QAction::triggered, this, &MainWindow::Find);

    connect(ui->actionAddFilter, &QAction::triggered, this, [this]() { AddFilter(QUuid::createUuid().toString()); });
    connect(ui->actionClearAllFilters, &QAction::triggered, this, &MainWindow::ClearAllFilters);
    ui->actionClearAllFilters->setDisabled(true);

    mFindRecord = new FindRecordWidget(this);
    connect(mFindRecord, &FindRecordWidget::FindRecords, this, &MainWindow::FindRecords);
    mLayout->addWidget(mFindRecord);
    mFindRecord->hide();

    mPreferencesEditor = new PreferencesEditor(this);
    connect(ui->actionPreferences, &QAction::triggered, this, [this]() {
        mPreferencesEditor->UpdateFields();
        mPreferencesEditor->show();
        mPreferencesEditor->raise();
        mPreferencesEditor->activateWindow();
    });

    QTimer::singleShot(0, [this] {
        try
        {
#ifdef _WIN32
            const auto tzdata = std::filesystem::current_path() / std::filesystem::path("tzdata");
#else
            const char *appDir = std::getenv("APPDIR");
            const auto tzdata = appDir ? std::filesystem::path(appDir) / std::filesystem::path("usr/share/tzdata")
                                       : std::filesystem::current_path() / std::filesystem::path("tzdata");
#endif
            loglib::Initialize(tzdata);
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
            mModel->Clear();
            ClearAllFilters();
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

bool MainWindow::event(QEvent *event)
{
    if (event->type() == QEvent::ApplicationFontChange)
    {
        QFont applicatioFont = qApp->font();
        mTableView->setFont(applicatioFont);
        applicatioFont.setBold(true);
        mTableView->horizontalHeader()->setFont(applicatioFont);
        return true;
    }
    return QMainWindow::event(event); // Handle other events normally
}

void MainWindow::OpenFiles()
{
    QStringList files = QFileDialog::getOpenFileNames(this, "Select Log Files", QString(), "All Files (*.*)");

    if (!files.isEmpty())
    {
        mModel->Clear();
        ClearAllFilters();
        for (const QString &file : files)
        {
            OpenFileInternal(file);
        }
    }
}

void MainWindow::OpenJsonLogs()
{
    QStringList files = QFileDialog::getOpenFileNames(this, "Select JSON Log Files", QString(), "All Files (*.*)");

    if (!files.isEmpty())
    {
        mModel->Clear();
        ClearAllFilters();
        std::unique_ptr<loglib::LogParser> jsonParser = loglib::LogFactory::Create(loglib::LogFactory::Parser::Json);
        for (const QString &file : files)
        {
            loglib::ParseResult result = jsonParser->Parse(file.toStdString());
            mModel->AddData(std::move(result.data));

            if (!result.errors.empty())
            {
                QMessageBox::warning(this, "Error Parsing JSON Logs", QString::fromStdString(result.errors[0]));
            }

            UpdateUi();
        }
    }
}

void MainWindow::SaveConfiguration()
{
    QString file = QFileDialog::getSaveFileName(this, "Save Configuration", QString(), "JSON (*.json);;All Files (*)");
    if (!file.isEmpty())
    {
        mModel->ConfigurationManager().Save(file.toStdString());
    }
}

void MainWindow::LoadConfiguration()
{
    QString file = QFileDialog::getOpenFileName(this, "Load Configuration", QString(), "JSON (*.json);;All Files (*)");
    if (!file.isEmpty())
    {
        try
        {
            mModel->Clear();
            mModel->ConfigurationManager().Load(file.toStdString());
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

void MainWindow::AddFilter(const QString filterId, const std::optional<loglib::LogConfiguration::LogFilter> &filter)
{
    if (mModel->rowCount() > 0)
    {
        auto filterEditor = new FilterEditor(*mModel, filterId, this);
        connect(filterEditor, &FilterEditor::FilterSubmitted, this, &MainWindow::FilterSubmitted);
        connect(filterEditor, &FilterEditor::FilterTimeStampSubmitted, this, &MainWindow::FilterTimeStampSubmitted);
        if (filter.has_value())
        {
            if (filter->type == loglib::LogConfiguration::LogFilter::Type::time)
            {
                filterEditor->Load(filter->row, *filter->filterBegin, *filter->filterEnd);
            }
            else
            {
                filterEditor->Load(
                    filter->row, QString::fromStdString(*filter->filterString), static_cast<int>(*filter->matchType)
                );
            }
        }
        filterEditor->show();
    }
}

void MainWindow::ClearAllFilters()
{
    mFilters.clear();
    mSortFilterProxyModel->SetFilterRules({});

    for (QAction *action : ui->menuFilters->actions())
    {
        if (!action->data().toString().isNull())
        {
            ui->menuFilters->removeAction(action);
            delete action;
        }
    }

    ui->actionClearAllFilters->setDisabled(true);
}

void MainWindow::ClearFilter(const QString &filterID)
{
    mFilters.erase(filterID.toStdString());
    UpdateFilters();

    unsigned filters = 0;
    for (QAction *action : ui->menuFilters->actions())
    {
        if (!action->data().toString().isNull())
        {
            if (action->data().toString() == filterID)
            {
                ui->menuFilters->removeAction(action);
                delete action;
            }
            else
            {
                filters++;
            }
        }
    }

    if (filters == 0)
    {
        ui->actionClearAllFilters->setDisabled(true);
    }
}

void MainWindow::FilterSubmitted(const QString &filterID, int row, const QString &filterString, int matchType)
{
    ClearFilter(filterID);

    loglib::LogConfiguration::LogFilter filter;
    filter.type = loglib::LogConfiguration::LogFilter::Type::string;
    filter.row = row;
    filter.filterString = filterString.toStdString();
    filter.matchType = static_cast<loglib::LogConfiguration::LogFilter::Match>(matchType);

    AddLogFilter(filterID, filter);
}

void MainWindow::FilterTimeStampSubmitted(const QString &filterID, int row, qint64 beginTimeStamp, qint64 endTimeStamp)
{
    ClearFilter(filterID);

    loglib::LogConfiguration::LogFilter filter;
    filter.type = loglib::LogConfiguration::LogFilter::Type::time;
    filter.row = row;
    filter.filterBegin = beginTimeStamp;
    filter.filterEnd = endTimeStamp;

    AddLogFilter(filterID, filter);
}

void MainWindow::OpenFileInternal(const QString &file)
{
    bool isConfiguration = true;
    try
    {
        mModel->ConfigurationManager().Load(file.toStdString());
    }
    catch (...)
    {
        isConfiguration = false;
    }

    if (!isConfiguration)
    {
        loglib::ParseResult result = loglib::LogFactory::Parse(file.toStdString());

        mModel->AddData(std::move(result.data));

        if (!result.errors.empty())
        {
            QMessageBox::warning(this, "Error Opening File", QString::fromStdString(result.errors[0]));
        }
    }

    UpdateUi();
}

void MainWindow::AddLogFilter(const QString &id, const loglib::LogConfiguration::LogFilter &filter)
{
    mFilters[id.toStdString()] = filter;
    UpdateFilters();

    QString title;
    if (filter.type == loglib::LogConfiguration::LogFilter::Type::time)
    {
        title = QString::fromStdString(
            loglib::UtcMicrosecondsToDateTimeString(*filter.filterBegin) + " - " +
            loglib::UtcMicrosecondsToDateTimeString(*filter.filterEnd)
        );
    }
    else
    {
        title = QString::fromStdString(*filter.filterString);
    }

    QMenu *menuItem = ui->menuFilters->addMenu(title);
    menuItem->menuAction()->setData(QVariant(id));

    QAction *editAction = menuItem->addAction("Edit");
    connect(editAction, &QAction::triggered, this, [this, id, filter]() { AddFilter(id, filter); });

    QAction *clearAction = menuItem->addAction("Clear");
    connect(clearAction, &QAction::triggered, this, [this, id]() { ClearFilter(id); });
    ui->actionClearAllFilters->setDisabled(false);
}

void MainWindow::UpdateFilters()
{

    std::vector<std::unique_ptr<FilterRule>> rules;
    for (const auto &filter : mFilters)
    {
        if (filter.second.type == loglib::LogConfiguration::LogFilter::Type::time)
        {
            rules.push_back(std::make_unique<TimeStampFilterRule>(
                filter.second.row, *filter.second.filterBegin, *filter.second.filterEnd
            ));
        }
        else
        {
            rules.push_back(std::make_unique<TextFilterRule>(
                filter.second.row, QString::fromStdString(*filter.second.filterString), *filter.second.matchType
            ));
        }
    }
    mSortFilterProxyModel->SetFilterRules(std::move(rules));
}
