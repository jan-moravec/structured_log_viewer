#include "main_window.hpp"
#include "./ui_main_window.h"

#include "appearance_control.hpp"
#include "filter_editor.hpp"
#include "qt_streaming_log_sink.hpp"
#include "streaming_control.hpp"

#include <loglib/json_parser.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_factory.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_processing.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/tailing_file_source.hpp>

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

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iterator>
#include <memory>
#include <system_error>
#include <vector>

namespace
{

// Locate the staged `tzdata/` directory. Tries (in order): next to the
// binary, the macOS Resources/ bundle layout, $APPDIR/usr/share/tzdata
// (Linux AppImage), then walks the CWD ancestor chain. On miss returns
// an empty path and populates `searched` for diagnostics.
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

// Diagnostic for "no tzdata found" matching common.cpp's shape.
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
    lines << QStringLiteral("Run the binary from a directory that has a sibling `tzdata/` "
                            "(deployed installs ship one next to the executable; `cmake/FetchDependencies.cmake` "
                            "stages it at `${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tzdata` for local builds).");
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

    mModel = new LogModel(mTableView);
    mTableView->setModel(mModel);
    mTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    mTableView->setSelectionMode(QAbstractItemView::MultiSelection);
    mTableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mTableView->setAlternatingRowColors(true);

    ApplyTableStyleSheet();

    // Two-stage proxy chain: the inner `StreamOrderProxyModel` flips
    // the row order when **Show newest lines first** is enabled (its
    // sort role is `InsertionOrderRole`, independent of any user
    // column sort); the outer `LogFilterModel` filters and supports
    // user-clicked column sorts using `SortRole`. The two layers never
    // share sort state, so a column sort and the newest-first flag
    // can coexist without fighting (the column sort wins on the
    // visible row order; clearing it falls back to the proxy's
    // reversed order).
    mStreamOrderProxyModel = new StreamOrderProxyModel(this);
    mStreamOrderProxyModel->setSourceModel(mModel);

    mSortFilterProxyModel = new LogFilterModel(this);
    mSortFilterProxyModel->setSourceModel(mStreamOrderProxyModel);
    mSortFilterProxyModel->setSortRole(SortRole);
    mTableView->setModel(mSortFilterProxyModel);
    mTableView->setSortingEnabled(true);
    mTableView->sortByColumn(-1, Qt::SortOrder::AscendingOrder);

    mTableView->resizeColumnsToContents();

    mTableView->horizontalHeader()->setStyleSheet(R"(QHeaderView::section { padding: 8px; font-weight: bold; })");
    mTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    mTableView->horizontalHeader()->resizeSections(QHeaderView::Stretch);
    mTableView->horizontalHeader()->setStretchLastSection(true);
    mTableView->horizontalHeader()->setHighlightSections(false);
    mTableView->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    mTableView->setShowGrid(true);
    mTableView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    mTableView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    connect(ui->actionOpen, &QAction::triggered, this, &MainWindow::OpenFiles);
    connect(ui->actionOpenJsonLogs, &QAction::triggered, this, &MainWindow::OpenJsonLogs);
    connect(ui->actionOpenLogStream, &QAction::triggered, this, &MainWindow::OpenLogStream);
    connect(ui->actionSaveConfiguration, &QAction::triggered, this, &MainWindow::SaveConfiguration);
    connect(ui->actionLoadConfiguration, &QAction::triggered, this, &MainWindow::LoadConfiguration);
    connect(ui->actionExit, &QAction::triggered, this, &MainWindow::close);

    connect(ui->actionCopy, &QAction::triggered, mTableView, &LogTableView::CopySelectedRowsToClipboard);
    connect(ui->actionFind, &QAction::triggered, this, &MainWindow::Find);

    connect(ui->actionAddFilter, &QAction::triggered, this, [this]() { AddFilter(QUuid::createUuid().toString()); });
    connect(ui->actionClearAllFilters, &QAction::triggered, this, &MainWindow::ClearAllFilters);
    ui->actionClearAllFilters->setDisabled(true);

    // Stream toolbar (PRD §6 *Toolbar*; task 5.3). Hidden until a stream
    // is opened; the same actions are also reachable from the **Stream**
    // menu (task 5.2).
    mStreamToolbar = addToolBar(tr("Stream"));
    mStreamToolbar->setObjectName("streamToolbar");
    mStreamToolbar->addAction(ui->actionPauseStream);
    mStreamToolbar->addAction(ui->actionFollowTail);
    mStreamToolbar->addAction(ui->actionStopStream);
    mStreamToolbar->setVisible(false);

    // The toolbar is hidden between sessions but the same actions live
    // in the **Stream** menu (`menuStream` in `main_window.ui`), which
    // stays reachable. Without an explicit disable, an idle-time menu
    // click on `actionPauseStream` (checkable) flips its checked state
    // before `TogglePauseStream`'s `IsStreamingActive` early-return
    // runs; the toggle then survives into the next session, which the
    // teardown path *cannot* unstick because `LogModel::Clear()` only
    // emits `streamingFinished` (where the toggle is reset) when the
    // prior session was still active — which it isn't after a `Detach()`
    // or a previous full teardown. Disabling the actions while idle
    // makes the only legal entry point the toolbar (which is in turn
    // gated on `IsStreamingActive`), so the checked state can never
    // leak across sessions. `UpdateStreamToolbarVisibility` keeps the
    // enabled flag in sync with the toolbar visibility for the rest of
    // the lifecycle.
    ui->actionPauseStream->setEnabled(false);
    ui->actionFollowTail->setEnabled(false);
    ui->actionStopStream->setEnabled(false);

    connect(ui->actionPauseStream, &QAction::toggled, this, &MainWindow::TogglePauseStream);
    connect(ui->actionStopStream, &QAction::triggered, this, &MainWindow::StopStream);

    // `actionFollowTail` is a stateless toggle observed by
    // `ScrollToNewestRowIfFollowing`; nothing to wire on its own
    // `toggled` signal. The user-scroll signals below auto-disengage /
    // auto-re-engage it (PRD 4.3.3). The "tail edge" the table view
    // tracks is bottom in the default orientation and top when
    // **Show newest lines first** is enabled — that flip lives in
    // `ApplyStreamingDisplayOrder` so this wiring stays orientation-
    // agnostic.
    connect(mTableView, &LogTableView::userScrolledAwayFromTail, this, [this]() {
        if (ui->actionFollowTail->isChecked())
        {
            ui->actionFollowTail->setChecked(false);
        }
    });
    connect(mTableView, &LogTableView::userScrolledToTail, this, [this]() {
        if (!ui->actionFollowTail->isChecked() && mModel->IsStreamingActive())
        {
            ui->actionFollowTail->setChecked(true);
        }
    });

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
    connect(mPreferencesEditor, &PreferencesEditor::streamingRetentionChanged, this, [this](qulonglong) {
        ApplyStreamingRetention();
    });
    connect(mPreferencesEditor, &PreferencesEditor::streamingDisplayOrderChanged, this, [this](bool) {
        ApplyStreamingDisplayOrder();
    });

    mStatusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(mStatusLabel);
    mStatusLabel->hide();

    connect(mModel, &LogModel::lineCountChanged, this, [this](qsizetype count) {
        mStreamingLineCount = count;
        UpdateStreamingStatus();
        // First-batch column auto-resize (PRD §6, task 5.9). `UpdateUi`
        // runs only on the first non-empty batch after `BeginStreaming`,
        // so initial widths fit pre-fill rows; thereafter the user
        // resizes manually (avoid yanking columns under the mouse).
        if (mLiveTailActive && !mFirstStreamingBatchSeen && count > 0)
        {
            mFirstStreamingBatchSeen = true;
            UpdateUi();
        }
        // Follow newest auto-scroll on every batch (PRD 4.3 / task 5.7).
        if (mModel->IsStreamingActive())
        {
            ScrollToNewestRowIfFollowing();
        }
    });
    connect(mModel, &LogModel::errorCountChanged, this, [this](qsizetype count) {
        mStreamingErrorCount = count;
        UpdateStreamingStatus();
    });
    connect(mModel, &LogModel::streamingFinished, this, [this](StreamingResult result) {
        mStreamingActive = false;
        mLiveTailActive = false;
        // Source is going away — drop the `Source unavailable` latch so
        // the post-session status-bar label doesn't inherit a stale
        // "waiting" state. Re-set naturally by the next stream's
        // first `Waiting` transition if one arrives.
        mSourceWaiting = false;
        // Reset Pause toggle so the next session starts unpaused.
        if (ui->actionPauseStream->isChecked())
        {
            const QSignalBlocker blocker(ui->actionPauseStream);
            ui->actionPauseStream->setChecked(false);
        }
        // Reset Follow newest to its `.ui` default (checked) so the next
        // live-tail session starts auto-scrolling — symmetric with the
        // Pause reset above. Without this, an `userScrolledAwayFromTail`
        // during the previous session would leave the toggle off and the
        // user would have to manually scroll to the bottom (or toggle
        // the action) to re-engage auto-scroll on the new stream. The
        // toggle has no `toggled` slot wired, so flipping it has no
        // side effects beyond changing the value
        // `ScrollToNewestRowIfFollowing` reads.
        if (!ui->actionFollowTail->isChecked())
        {
            const QSignalBlocker blocker(ui->actionFollowTail);
            ui->actionFollowTail->setChecked(true);
        }
        SetConfigurationUiEnabled(true);
        UpdateStreamToolbarVisibility();
        UpdateUi();
        UpdateStreamingStatus();
        // Only `Success` produces a post-parse error summary; cancellation
        // hides it and `Failed` surfaces independently.
        if (result == StreamingResult::Success)
        {
            ShowParseErrors("Error Parsing Logs", mModel->StreamingErrors());
        }
        mStreamingFileName.clear();
    });
    connect(mModel, &LogModel::rotationDetected, this, &MainWindow::OnRotationDetected);
    connect(mModel, &LogModel::sourceStatusChanged, this, &MainWindow::OnSourceStatusChanged);

    // Pull the persisted streaming preferences into the model and the
    // proxy on startup so the first `OpenLogStream` honours the user's
    // choices (task 5.12 for retention; symmetric wiring for the
    // newest-first toggle added later).
    StreamingControl::LoadConfiguration();
    ApplyStreamingRetention();
    ApplyStreamingDisplayOrder();

    QTimer::singleShot(0, [this] {
        // qCritical() instead of a modal dialog: offscreen Qt (CI / apptest) hangs on modals.
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

    if (!mimeData->hasUrls())
    {
        return;
    }

    const QList<QUrl> urlList = mimeData->urls();
    if (urlList.isEmpty())
    {
        return;
    }

    mModel->Clear();
    ClearAllFilters();

    std::vector<std::string> errors;

    // Mirror `OpenFilesWithParser`: stream a single dropped JSON log so the
    // GUI stays responsive and shows progress instead of freezing on the
    // synchronous `LogFactory::Parse`. Configuration files and multi-file
    // drops keep the existing synchronous path.
    if (urlList.size() == 1)
    {
        const QString singleFile = urlList.front().toLocalFile();

        bool isConfiguration = true;
        try
        {
            mModel->ConfigurationManager().Load(singleFile.toStdString());
        }
        catch (...)
        {
            isConfiguration = false;
        }

        if (isConfiguration)
        {
            UpdateUi();
        }
        else
        {
            const loglib::JsonParser jsonParser;
            bool streamed = false;
            try
            {
                if (jsonParser.IsValid(singleFile.toStdString()))
                {
                    streamed = OpenJsonStreaming(singleFile, errors);
                }
            }
            catch (...)
            {
                streamed = false;
            }

            if (!streamed)
            {
                OpenFileInternal(singleFile, errors);
            }
        }
    }
    else
    {
        for (const QUrl &url : urlList)
        {
            OpenFileInternal(url.toLocalFile(), errors);
        }
    }

    // Streaming surfaces its own error summary from `streamingFinished`;
    // anything queued in `errors` here is from the synchronous fallback.
    ShowParseErrors("Error Opening File", errors);

    event->acceptProposedAction();
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
        // Stylesheet encodes palette-derived colors; refresh on theme change.
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

    // Stream only single-file JSON opens; the sink holds one generation.
    const bool canStream = (dynamic_cast<loglib::JsonParser *>(parser.get()) != nullptr) && files.size() == 1;

    mModel->Clear();
    ClearAllFilters();

    if (canStream)
    {
        std::vector<std::string> errors;
        const bool started = OpenJsonStreaming(files.front(), errors);
        if (!started)
        {
            // Fall back to synchronous JSON so the user still gets a summary.
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
        // The streaming path shows its summary from `streamingFinished`.
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
    // Open on the GUI thread so file-open errors are synchronous.
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

    // Snapshot the configuration before handing it to the parser: the
    // worker reads it lock-free, and a UI-gate-skipping edit cannot then
    // affect the in-flight parse.
    auto cfg = std::make_shared<const loglib::LogConfiguration>(mModel->Configuration());

    mStreamingFileName = QFileInfo(file).fileName();
    if (!logFile)
    {
        // Fail early before flipping any streaming UI state.
        errors.push_back(std::string("Failed to open '") + file.toStdString() + "' for streaming");
        return false;
    }

    mStreamingActive = true;
    mLiveTailActive = false;
    mStreamingLineCount = 0;
    mStreamingErrorCount = 0;
    mFirstStreamingBatchSeen = false;
    SetConfigurationUiEnabled(false);
    UpdateStreamingStatus();
    UpdateStreamToolbarVisibility();

    // Borrow the same `LogFile*` the model is about to take ownership of;
    // emitted string_view values point into its mmap. Capturing before the
    // `std::move` is safe — the model keeps the file alive until `Clear()`
    // joins the worker.
    loglib::LogFile *parseFile = logFile.get();
    QtStreamingLogSink *sink = mModel->Sink();

    loglib::ParserOptions options;
    options.configuration = std::move(cfg);

    // The model owns spawning the worker and parking the future, so
    // `Clear()` always has a future to wait on.
    mModel->BeginStreaming(
        std::move(logFile),
        [sink, parseFile, options = std::move(options)](loglib::StopToken stopToken) mutable {
            options.stopToken = stopToken;
            loglib::JsonParser parser;
            parser.ParseStreaming(*parseFile, *sink, options);
        }
    );

    return true;
}

void MainWindow::OpenLogStream()
{
    const QString file = QFileDialog::getOpenFileName(this, "Open Log Stream...", QString(), "All Files (*.*)");
    if (file.isEmpty())
    {
        return;
    }

    // Construct the source on the GUI thread so open errors are
    // synchronous (PRD 4.1.7 — failures stay in the previous state, no
    // streaming UI flip). Pre-fill needs to know the retention cap so it
    // back-reads at most that many lines off disk.
    const size_t retention =
        (mModel->RetentionCap() != 0) ? mModel->RetentionCap() : StreamingControl::RetentionLines();

    std::unique_ptr<loglib::TailingFileSource> source;
    try
    {
        source = std::make_unique<loglib::TailingFileSource>(std::filesystem::path(file.toStdString()), retention);
    }
    catch (const std::exception &e)
    {
        ShowParseErrors(
            "Error Opening Log Stream",
            {std::string("Failed to open '") + file.toStdString() + "' for streaming: " + e.what()}
        );
        return;
    }

    // Mirror the static-open reset order from `OpenFilesWithParser` so
    // residual state (filters, Find match, paused buffer) does not leak
    // into the new session.
    mModel->Clear();
    ClearAllFilters();

    mStreamingFileName = QFileInfo(file).fileName();
    mStreamingActive = true;
    mLiveTailActive = true;
    mStreamingLineCount = 0;
    mStreamingErrorCount = 0;
    mFirstStreamingBatchSeen = false;
    SetConfigurationUiEnabled(false);
    UpdateStreamingStatus();
    UpdateStreamToolbarVisibility();

    // Snapshot the configuration so the worker reads it lock-free
    // (matches `OpenJsonStreaming`).
    auto cfg = std::make_shared<const loglib::LogConfiguration>(mModel->Configuration());

    loglib::ParserOptions options;
    options.configuration = std::move(cfg);

    mModel->BeginStreaming(std::move(source), std::move(options));
}

void MainWindow::TogglePauseStream(bool paused)
{
    if (!mModel->IsStreamingActive())
    {
        return;
    }
    if (paused)
    {
        mModel->Sink()->Pause();
    }
    else
    {
        mModel->Sink()->Resume();
    }
    UpdateStreamingStatus();
}

void MainWindow::StopStream()
{
    if (!mModel->IsStreamingActive())
    {
        return;
    }
    // `LogModel::Detach()` runs the mandatory PRD 4.7.2.i teardown
    // sequence (source `Stop()` → sink `RequestStop()` → worker
    // `waitForFinished()` → flush paused buffer → `DropPendingBatches()`)
    // and emits a compensating `streamingFinished(Cancelled)` so the
    // GUI gating reopens — but leaves the visible rows intact so the
    // user can keep sorting / filtering / searching / copying after
    // Stop (PRD 4.7.1). `mStreamingFileName` is cleared so a subsequent
    // "Open…" does not inherit the old filename in status text.
    mModel->Detach();
    mStreamingFileName.clear();
}

void MainWindow::OnRotationDetected()
{
    // PRD §6 *Rotation indicator*: brief 3 s `— rotated` flash on the
    // status label. A `QTimer::singleShot` clears the flag.
    mRotationFlashActive = true;
    UpdateStreamingStatus();
    QTimer::singleShot(3000, this, [this]() {
        mRotationFlashActive = false;
        UpdateStreamingStatus();
    });
}

void MainWindow::OnSourceStatusChanged(loglib::SourceStatus status)
{
    // PRD 4.8.8 / §6 *Status bar*: latch the `Waiting` state so the
    // label keeps showing "Source unavailable …" between poll ticks
    // (the status callback is edge-triggered, not level-triggered).
    mSourceWaiting = (status == loglib::SourceStatus::Waiting);
    UpdateStreamingStatus();
}

void MainWindow::SetConfigurationUiEnabled(bool enabled)
{
    // The parser holds an immutable snapshot; gate edits while streaming.
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

    QString text;
    if (!mLiveTailActive)
    {
        // Static streaming-parse path (existing behaviour).
        text = QString("Parsing %1 - %2 lines, %3 errors")
                   .arg(mStreamingFileName)
                   .arg(mStreamingLineCount)
                   .arg(mStreamingErrorCount);
    }
    else if (mSourceWaiting)
    {
        // Source unavailable: the file disappeared during a
        // delete-then-recreate rotation (PRD 4.8.8 / §6 *Status bar*).
        // Takes precedence over the Paused variant because Paused is
        // a user-initiated UI state while Source unavailable surfaces
        // an actionable environmental problem.
        text = QString("Source unavailable - last seen %1 - %2 lines, %3 errors")
                   .arg(mStreamingFileName)
                   .arg(mStreamingLineCount)
                   .arg(mStreamingErrorCount);
    }
    else if (mModel->Sink() && mModel->Sink()->IsPaused())
    {
        // Paused: PRD §6 status-bar wording.
        const auto buffered = static_cast<qsizetype>(mModel->Sink()->PausedLineCount());
        text = QString("Paused - %1 lines, %2 buffered").arg(mStreamingLineCount).arg(buffered);
    }
    else
    {
        // Running.
        text = QString("Streaming %1 - %2 lines, %3 errors")
                   .arg(mStreamingFileName)
                   .arg(mStreamingLineCount)
                   .arg(mStreamingErrorCount);
    }

    // Paused-drop telemetry (PRD 4.2.2.iv). The counter is session-scoped
    // and increments only while paused; once set, it stays non-zero until
    // the stream is stopped so the user keeps seeing "lines were lost"
    // after Resume. Appended to both Paused and Running variants of the
    // label; the static-streaming path does not pause in practice (the
    // toolbar is hidden), so the check is live-tail-only.
    if (mLiveTailActive && mModel->Sink())
    {
        const auto dropped = static_cast<qsizetype>(mModel->Sink()->PausedDropCount());
        if (dropped > 0)
        {
            text += QString(", %1 dropped while paused").arg(dropped);
        }
    }

    if (mLiveTailActive && mRotationFlashActive)
    {
        text += " - rotated";
    }

    mStatusLabel->setText(text);
    mStatusLabel->show();
}

void MainWindow::UpdateStreamToolbarVisibility()
{
    // Use the MainWindow's own `mStreamingActive` (set on entry to
    // `OpenLogStream` / `OpenJsonStreaming`, cleared in the
    // `streamingFinished` slot before this call) rather than
    // `mModel->IsStreamingActive()`. The model flag is only set inside
    // `BeginStreaming`, which runs *after* the existing call sites here
    // — so reading the model flag here would always return `false` on
    // open and the toolbar would never become visible until something
    // else triggered another call.
    const bool visible = mStreamingActive && mLiveTailActive;
    if (mStreamToolbar)
    {
        mStreamToolbar->setVisible(visible);
    }
    // Gate the menu actions on the toolbar's visibility so a Stream-menu
    // click while idle (the menu items remain reachable even with the
    // toolbar hidden) cannot pre-flip a checkable action's state into
    // the next session. Without this guard `actionPauseStream` can land
    // checked before `BeginStreaming` runs, leaving the toolbar showing
    // "Paused" while the sink is genuinely running.
    //
    // The companion reset of the action's *checked* state at session
    // boundaries lives in the `streamingFinished` slot above (see the
    // `QSignalBlocker(actionPauseStream)` block) — keeping the logic in
    // one place avoids two slightly-different uncheck pathways racing
    // against each other.
    ui->actionPauseStream->setEnabled(visible);
    ui->actionFollowTail->setEnabled(visible);
    ui->actionStopStream->setEnabled(visible);
}

void MainWindow::ScrollToNewestRowIfFollowing()
{
    if (!ui->actionFollowTail->isChecked())
    {
        return;
    }
    const int sourceRowCount = mModel->rowCount();
    if (sourceRowCount <= 0)
    {
        return;
    }
    // PRD 4.3.4 — scroll to the most-recently-appended source row even
    // when sorted by a non-time column. Mapping through both proxy
    // layers (StreamOrder → LogFilter) makes the visual scroll land
    // on the correct row under reverse-order, sort, and filter.
    const QModelIndex sourceIndex = mModel->index(sourceRowCount - 1, 0);
    const QModelIndex midIndex = mStreamOrderProxyModel->mapFromSource(sourceIndex);
    const QModelIndex proxyIndex = mSortFilterProxyModel->mapFromSource(midIndex);
    if (!proxyIndex.isValid())
    {
        return;
    }
    // In newest-first mode the most-recently-appended row sits at the
    // *top* of the view (proxy row 0), so Follow newest must scroll to
    // the top edge instead of the bottom. The `LogTableView`'s
    // `TailEdge` is kept in sync with this orientation by
    // `ApplyStreamingDisplayOrder` so the user-scroll detection also
    // tracks the right edge.
    const auto position = mStreamOrderProxyModel->IsReversed() ? QAbstractItemView::PositionAtTop
                                                               : QAbstractItemView::PositionAtBottom;
    mTableView->scrollTo(proxyIndex, position);
}

void MainWindow::ApplyStreamingRetention()
{
    mModel->SetRetentionCap(StreamingControl::RetentionLines());
}

void MainWindow::ApplyStreamingDisplayOrder()
{
    const bool newestFirst = StreamingControl::IsNewestFirst();
    mStreamOrderProxyModel->SetReversed(newestFirst);
    mTableView->SetTailEdge(newestFirst ? LogTableView::TailEdge::Top : LogTableView::TailEdge::Bottom);
    // Alternating row colours are keyed off the **visual** row index
    // by Qt — perfect when new rows append at the bottom and existing
    // rows keep their visual positions, but newest-first inserts at
    // proxy row 0 and shifts every existing row's parity on every
    // batch, so each arriving line flips every visible row's tone.
    // We tried overriding the per-cell `Alternate` flag in a custom
    // delegate to pin colours to source-row parity; in practice the
    // CSS-based table style (`alternate-background-color`) bypassed
    // the override and the rows still flickered, so we just turn the
    // alternation off in newest-first mode and accept a single base
    // colour there. Default mode keeps the visual reading aid.
    mTableView->setAlternatingRowColors(!newestFirst);
    // Re-pin the view to the (potentially new) tail edge if Follow
    // tail is currently engaged so the user does not have to scroll
    // manually after toggling the preference. Keeps behaviour
    // consistent with the implicit `scrollTo` that `lineCountChanged`
    // would otherwise need to wait for.
    if (mModel->IsStreamingActive())
    {
        ScrollToNewestRowIfFollowing();
    }
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
    // Try as a configuration first; fall back to log parsing on failure.
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
