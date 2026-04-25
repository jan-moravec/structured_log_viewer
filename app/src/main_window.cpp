#include "main_window.hpp"
#include "./ui_main_window.h"

#include "appearance_control.hpp"
#include "filter_editor.hpp"
#include "qt_streaming_log_sink.hpp"

#include <loglib/json_parser.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_factory.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_processing.hpp>

#include <QCheckBox>
#include <QCoreApplication>
#include <QDebug>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QTableView>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <exception>
#include <iterator>
#include <memory>
#include <stop_token>

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

    ApplyTableStyleSheet();

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

    // Status-bar plumbing for the streaming progress label (PRD req. 4.3.29).
    // The label is permanently a child of the status bar; visibility is toggled
    // through the text content (empty -> hidden) so the layout doesn't reflow on
    // every update tick.
    mStatusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(mStatusLabel);
    mStatusLabel->hide();

    mStreamingWatcher = new QFutureWatcher<void>(this);

    // Wire the LogModel streaming signals into the status-bar updater and the
    // post-parse summary slot. lineCountChanged also fires for batches that only
    // carry errors (so the label still ticks); errorCountChanged only fires when
    // the batch actually contained errors. streamingFinished is the single
    // terminal slot — re-enables config UI, clears the label, surfaces the
    // QMessageBox summary on success.
    connect(mModel, &LogModel::lineCountChanged, this, [this](qsizetype count) {
        mStreamingLineCount = count;
        UpdateStreamingStatus();
    });
    connect(mModel, &LogModel::errorCountChanged, this, [this](qsizetype count) {
        mStreamingErrorCount = count;
        UpdateStreamingStatus();
    });
    connect(mModel, &LogModel::streamingFinished, this, [this](bool cancelled) {
        mStreamingActive = false;
        SetConfigurationUiEnabled(true);
        UpdateUi();
        // mStatusLabel is hidden by UpdateStreamingStatus once mStreamingActive is false.
        UpdateStreamingStatus();
        if (!cancelled)
        {
            ShowParseErrors("Error Parsing Logs", mModel->StreamingErrors());
        }
        mStreamingFileName.clear();
    });

    QTimer::singleShot(0, [this] {
        try
        {
#ifdef _WIN32
            const auto tzdata = std::filesystem::current_path() / std::filesystem::path("tzdata");
#elif defined(__APPLE__)
            // Inside a .app bundle, applicationDirPath() is <bundle>/Contents/MacOS.
            // tzdata is shipped as <bundle>/Contents/Resources/tzdata.
            const auto contentsDir =
                std::filesystem::path(QCoreApplication::applicationDirPath().toStdString()).parent_path();
            const auto tzdata = contentsDir / "Resources" / "tzdata";
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
        const QList<QUrl> urlList = mimeData->urls();
        if (urlList.isEmpty())
        {
            return;
        }

        mModel->Clear();
        ClearAllFilters();

        std::vector<std::string> errors;
        for (const QUrl &url : urlList)
        {
            OpenFileInternal(url.toLocalFile(), errors);
        }
        ShowParseErrors("Error Opening File", errors);

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
    switch (event->type())
    {
    case QEvent::ApplicationFontChange: {
        QFont applicationFont = qApp->font();
        mTableView->setFont(applicationFont);
        applicationFont.setBold(true);
        mTableView->horizontalHeader()->setFont(applicationFont);
        break;
    }
    case QEvent::ApplicationPaletteChange:
    case QEvent::ThemeChange:
    case QEvent::StyleChange:
        // The table stylesheet encodes dark/light colors derived from the current palette, so
        // it must be refreshed whenever the user switches style or system theme.
        ApplyTableStyleSheet();
        break;
    default:
        break;
    }
    return QMainWindow::event(event);
}

void MainWindow::OpenFiles()
{
    OpenFilesWithParser("Select Log Files", nullptr);
}

void MainWindow::OpenJsonLogs()
{
    OpenFilesWithParser("Select JSON Log Files", loglib::LogFactory::Create(loglib::LogFactory::Parser::Json));
}

void MainWindow::OpenFilesWithParser(const QString &dialogTitle, std::unique_ptr<loglib::LogParser> parser)
{
    const QStringList files = QFileDialog::getOpenFileNames(this, dialogTitle, QString(), "All Files (*.*)");
    if (files.isEmpty())
    {
        return;
    }

    // Use the streaming path when the chosen parser is the JSON parser AND only
    // one file was selected — the streaming pipeline is a per-file affair (the
    // sink owns one parse generation at a time) so multi-file selection still
    // routes through the legacy synchronous loop. The single-file streaming case
    // is the dominant UX (open one big log, watch it stream in) and is the one
    // the PRD targets (req. 4.3.29).
    const bool canStream = (dynamic_cast<loglib::JsonParser *>(parser.get()) != nullptr) && files.size() == 1;

    mModel->Clear();
    ClearAllFilters();

    if (canStream)
    {
        std::vector<std::string> errors;
        const bool started = OpenJsonStreaming(files.front(), errors);
        if (!started)
        {
            // Streaming setup failed (e.g. mmap open error). Fall back to the
            // synchronous JSON path so the user still sees an error summary —
            // and so we don't silently drop the open. The fallback intentionally
            // shares the post-parse summary path with the streaming success
            // case to keep the UX consistent.
            try
            {
                loglib::ParseResult result = parser->Parse(files.front().toStdString());
                mModel->AddData(std::move(result.data));
                errors.insert(
                    errors.end(),
                    std::make_move_iterator(result.errors.begin()),
                    std::make_move_iterator(result.errors.end())
                );
            }
            catch (const std::exception &e)
            {
                errors.push_back(std::string("Failed to parse '") + files.front().toStdString() + "': " + e.what());
            }
            UpdateUi();
            ShowParseErrors("Error Parsing Logs", errors);
        }
        // On the streaming-started path the post-parse summary is shown from
        // the streamingFinished slot (which fires after the parser has fully
        // drained — including any final empty terminal batch — see PRD req.
        // 4.3.26a / 4.3.29).
        return;
    }

    std::vector<std::string> errors;
    for (const QString &file : files)
    {
        if (parser)
        {
            try
            {
                loglib::ParseResult result = parser->Parse(file.toStdString());
                mModel->AddData(std::move(result.data));
                errors.insert(
                    errors.end(),
                    std::make_move_iterator(result.errors.begin()),
                    std::make_move_iterator(result.errors.end())
                );
            }
            catch (const std::exception &e)
            {
                errors.push_back(std::string("Failed to parse '") + file.toStdString() + "': " + e.what());
            }
        }
        else
        {
            OpenFileInternal(file, errors);
        }
    }

    UpdateUi();
    ShowParseErrors("Error Parsing Logs", errors);
}

bool MainWindow::OpenJsonStreaming(const QString &file, std::vector<std::string> &errors)
{
    // Open the LogFile up-front on the GUI thread so file-open errors surface
    // synchronously and the caller can fall back to the synchronous path. Once
    // ownership transfers into LogModel::BeginStreaming, the model owns the
    // mmap for the lifetime of the parse.
    std::unique_ptr<loglib::LogFile> logFile;
    try
    {
        logFile = std::make_unique<loglib::LogFile>(file.toStdString());
    }
    catch (const std::exception &e)
    {
        errors.push_back(std::string("Failed to open '") + file.toStdString() + "': " + e.what());
        return false;
    }

    // Snapshot the configuration BEFORE handing it to Stage B. The shared_ptr<const>
    // lets the worker thread read it lock-free for the lifetime of the parse;
    // the GUI thread is gated against editing it via SetConfigurationUiEnabled
    // (PRD req. 4.2.21 / 4.3.29). Snapshotting up-front (rather than passing
    // mModel->Configuration() by reference) means a configuration edit that
    // sneaks past the UI gate cannot still affect the in-flight parse.
    auto cfg = std::make_shared<const loglib::LogConfiguration>(mModel->Configuration());

    mStreamingFileName = QFileInfo(file).fileName();
    mStreamingActive = true;
    mStreamingLineCount = 0;
    mStreamingErrorCount = 0;
    SetConfigurationUiEnabled(false);
    UpdateStreamingStatus();

    // BeginStreaming installs the file on the model's LogTable and bumps the
    // QtStreamingLogSink generation, returning the freshly-installed stop_token.
    const std::stop_token stopToken = mModel->BeginStreaming(std::move(logFile));
    QtStreamingLogSink *sink = mModel->Sink();

    // Hand the parser a borrowed reference to the *same* LogFile the model
    // owns, instead of opening a second mmap on the worker thread. Stage B
    // emits std::string_view-typed LogValues that point into the file's mmap
    // (PRD req. 4.1.6/4.1.15a), so the file must outlive every LogLine the
    // sink delivers to LogTable. Sharing the model's LogFile is safe because
    // the parser only ever reads the mmap and only mutates the line-offset
    // table via Stage C → sink → LogTable::AppendBatch (i.e. the GUI thread).
    loglib::LogFile *parseFile = nullptr;
    if (!mModel->Table().Data().Files().empty())
    {
        parseFile = mModel->Table().Data().Files().front().get();
    }
    if (parseFile == nullptr)
    {
        errors.push_back(std::string("Failed to install '") + file.toStdString() + "' on the streaming model");
        mStreamingActive = false;
        SetConfigurationUiEnabled(true);
        UpdateStreamingStatus();
        return false;
    }

    loglib::JsonParserOptions options;
    options.stopToken = stopToken;
    options.configuration = std::move(cfg);

    // QtConcurrent::run launches on the global thread pool and returns a
    // QFuture<void>. The future is owned by mStreamingWatcher so a second open
    // can observe the previous parse's finalisation. The actual cancellation
    // path is Clear()->RequestStop()->stop_token; the watcher is purely a
    // lifetime tracker (PRD req. 4.3.28 / 4.3.29).
    QFuture<void> future = QtConcurrent::run([sink, options = std::move(options), parseFile]() {
        try
        {
            loglib::JsonParser parser;
            parser.ParseStreaming(*parseFile, *sink, options);
        }
        catch (const std::exception &e)
        {
            // Surface a synthetic terminal batch carrying the open/parse error
            // so the GUI sees a single contract for failure too. The sink's
            // generation check still applies; if cancellation already fired,
            // this never reaches the model.
            loglib::StreamedBatch errorBatch;
            errorBatch.errors.emplace_back(std::string("Streaming parse failed: ") + e.what());
            sink->OnBatch(std::move(errorBatch));
            sink->OnFinished(false);
        }
    });
    mStreamingWatcher->setFuture(future);

    return true;
}

void MainWindow::SetConfigurationUiEnabled(bool enabled)
{
    // PRD req. 4.2.21 / 4.3.29 — while a streaming parse is in flight, every
    // affordance that mutates the LogConfiguration must be disabled. The parser
    // thread holds an immutable snapshot, so a user edit during streaming would
    // either silently affect only post-streaming rows or trigger an expensive
    // whole-data re-parse. Re-enabled from the streamingFinished slot.
    ui->actionLoadConfiguration->setEnabled(enabled);
    ui->actionSaveConfiguration->setEnabled(enabled);
    ui->actionPreferences->setEnabled(enabled);
}

void MainWindow::UpdateStreamingStatus()
{
    if (!mStreamingActive)
    {
        mStatusLabel->clear();
        mStatusLabel->hide();
        return;
    }
    QString text = QString("Parsing %1 - %2 lines, %3 errors")
                       .arg(mStreamingFileName)
                       .arg(mStreamingLineCount)
                       .arg(mStreamingErrorCount);
    mStatusLabel->setText(text);
    mStatusLabel->show();
}

void MainWindow::ShowParseErrors(const QString &title, const std::vector<std::string> &errors)
{
    if (errors.empty())
    {
        return;
    }

    constexpr size_t kMaxErrorsShown = 20;
    QString message;
    const size_t shown = std::min(errors.size(), kMaxErrorsShown);
    for (size_t i = 0; i < shown; ++i)
    {
        message += QString::fromStdString(errors[i]) + QLatin1Char('\n');
    }
    if (errors.size() > kMaxErrorsShown)
    {
        message += QString("... and %1 more error(s).").arg(errors.size() - kMaxErrorsShown);
    }

    QMessageBox::warning(this, title, message);
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

void MainWindow::OpenFileInternal(const QString &file, std::vector<std::string> &errors)
{
    // Attempt to load the file as a configuration first; if it fails, fall back to log parsing.
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
        try
        {
            loglib::ParseResult result = loglib::LogFactory::Parse(file.toStdString());
            mModel->AddData(std::move(result.data));
            errors.insert(
                errors.end(),
                std::make_move_iterator(result.errors.begin()),
                std::make_move_iterator(result.errors.end())
            );
        }
        catch (const std::exception &e)
        {
            errors.push_back(std::string("Failed to open '") + file.toStdString() + "': " + e.what());
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

void MainWindow::ApplyTableStyleSheet()
{
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
