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
#include <QStringList>
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
#include <system_error>
#include <vector>

namespace
{

// Resolve the on-disk `tzdata/` directory the date library reads at startup.
// Search order (first hit wins):
//   1. `<applicationDirPath>/tzdata`        — the canonical layout on every
//      platform: cmake/FetchDependencies.cmake stages tzdata next to the
//      built executable (`${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tzdata`), and
//      installed/deployed bundles ship it in the same relative spot. This
//      is the path that lets the binary run from anywhere on disk without
//      needing a particular working directory.
//   2. `<applicationDirPath>/../Resources/tzdata` — macOS .app bundle layout
//      (`applicationDirPath()` is `<bundle>/Contents/MacOS`, tzdata sits at
//      `<bundle>/Contents/Resources/tzdata`).
//   3. `$APPDIR/usr/share/tzdata`           — Linux AppImage layout.
//   4. Walk up the current-directory ancestor chain looking for a sibling
//      `tzdata/` (mirrors `InitializeTimezoneData()` in
//      `test/lib/src/common.cpp`). Lets manual invocations succeed when the
//      CWD happens to live inside the build tree.
//
// On miss returns an empty path; `searched` is populated with every candidate
// the lookup tried so the caller can quote it in the failure diagnostic. The
// stop conditions on the CWD walk match common.cpp:
//   * `parent.empty()` — POSIX, parent of `/` is `""`;
//   * `parent == path` — Windows, `path("C:\\").parent_path()` returns
//     `C:\\` itself, so the walk would otherwise spin forever.
std::filesystem::path FindTzdata(std::vector<std::filesystem::path> &searched)
{
    auto pushAndCheck = [&searched](std::filesystem::path candidate) -> bool {
        std::error_code ec;
        const bool exists = std::filesystem::exists(candidate, ec);
        searched.push_back(std::move(candidate));
        return exists && !ec;
    };

    const auto appDir = std::filesystem::path(QCoreApplication::applicationDirPath().toStdString());
    if (!appDir.empty() && pushAndCheck(appDir / "tzdata"))
    {
        return searched.back();
    }

#ifdef __APPLE__
    if (!appDir.empty() && pushAndCheck(appDir.parent_path() / "Resources" / "tzdata"))
    {
        return searched.back();
    }
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
    if (const char *appImageDir = std::getenv("APPDIR"))
    {
        if (pushAndCheck(std::filesystem::path(appImageDir) / "usr/share/tzdata"))
        {
            return searched.back();
        }
    }
#endif

    std::error_code cwdEc;
    auto walk = std::filesystem::current_path(cwdEc);
    if (!cwdEc)
    {
        while (true)
        {
            if (pushAndCheck(walk / "tzdata"))
            {
                return searched.back();
            }
            const auto parent = walk.parent_path();
            if (parent.empty() || parent == walk)
            {
                break;
            }
            walk = parent;
        }
    }

    return {};
}

// Format a multi-line diagnostic listing every searched candidate, matching
// the shape of `FAIL(...)` in test/lib/src/common.cpp's
// `InitializeTimezoneData()` so support requests against either codepath
// surface the same actionable detail.
QString FormatTzdataNotFoundMessage(const std::vector<std::filesystem::path> &searched)
{
    QStringList lines;
    lines << QStringLiteral("Could not find the `tzdata/` directory required to initialize the timezone database.");
    lines << QStringLiteral("Searched the following candidate locations (in order):");
    for (const auto &p : searched)
    {
        lines << QStringLiteral("  - %1").arg(QString::fromStdString(p.string()));
    }
    lines << QString();
    lines << QStringLiteral(
        "Run the binary from a directory that has a sibling `tzdata/` "
        "(deployed installs ship one next to the executable; `cmake/FetchDependencies.cmake` "
        "stages it at `${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tzdata` for local builds)."
    );
    return lines.join(QLatin1Char('\n'));
}

} // namespace

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

    // Status-bar plumbing for the streaming progress label. The label is a
    // permanent child of the status bar; visibility is toggled by setting empty
    // text so the layout doesn't reflow on every update tick.
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
        // Resolve the tzdata directory and initialize the date library. On
        // failure we deliberately emit a stderr diagnostic via `qCritical()`
        // and exit instead of popping a modal `QMessageBox::critical(...)`:
        // under `QT_QPA_PLATFORM=offscreen` (CI / `apptest`) a modal dialog
        // blocks the event loop indefinitely with no way to dismiss it, and
        // we have observed it also crash inside `QWidget::setParent` when
        // the QApplication / parent widget state is not yet fully settled
        // mid-`QTimer::singleShot` dispatch. `qCritical()` always lands on
        // stderr regardless of platform, so the user (or the CI log) sees
        // exactly which paths the lookup tried and why startup aborted.
        std::vector<std::filesystem::path> searched;
        const auto tzdata = FindTzdata(searched);

        if (tzdata.empty())
        {
            const QString message = FormatTzdataNotFoundMessage(searched);
            qCritical().noquote() << "Fatal:" << message;
            QApplication::exit(1);
            return;
        }

        try
        {
            loglib::Initialize(tzdata);
        }
        catch (std::exception &e)
        {
            qCritical().noquote() << "Fatal: failed to initialize timezone database at"
                                  << QString::fromStdString(tzdata.string()) << ":" << e.what();
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

    // Use the streaming path only when the chosen parser is JSON AND a single
    // file was selected — the streaming pipeline is a per-file affair (the
    // sink owns one parse generation at a time), so multi-file selection still
    // goes through the synchronous loop. Single-file streaming is the dominant
    // UX (open one big log, watch it stream in).
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
        // the `streamingFinished` slot, which fires after the parser drains
        // (including the final empty terminal batch).
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

    // Snapshot the configuration BEFORE handing it to the parser. The
    // `shared_ptr<const>` lets the worker read it lock-free; the GUI is
    // gated against editing it via `SetConfigurationUiEnabled`. Snapshotting
    // up-front means a configuration edit that sneaks past the UI gate
    // cannot still affect the in-flight parse.
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

    // Hand the parser a borrowed reference to the *same* `LogFile` the model
    // owns instead of opening a second mmap on the worker. The parser emits
    // `string_view`-typed `LogValue`s that point directly into the file's
    // mmap, so the file must outlive every `LogLine` the sink hands to
    // `LogTable`. Sharing is safe: the worker only reads the mmap; the
    // line-offset table is mutated only from the GUI thread via
    // `LogTable::AppendBatch`.
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

    loglib::ParserOptions options;
    options.stopToken = stopToken;
    options.configuration = std::move(cfg);

    // `QtConcurrent::run` launches on the global thread pool and returns a
    // `QFuture<void>`. The future is owned by `mStreamingWatcher` so a second
    // open can observe the previous parse's finalisation. The cancellation
    // path is `Clear() -> RequestStop() -> stop_token`; the watcher is just a
    // lifetime tracker.
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
    // While a streaming parse is in flight, every affordance that mutates
    // `LogConfiguration` must be disabled. The parser holds an immutable
    // snapshot, so a user edit during streaming would either silently affect
    // only post-streaming rows or trigger an expensive whole-data re-parse.
    // Re-enabled from the `streamingFinished` slot.
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
