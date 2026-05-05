#include "main_window.hpp"
#include "./ui_main_window.h"

#include "appearance_control.hpp"
#include "filter_editor.hpp"
#include "qt_streaming_log_sink.hpp"
#include "streaming_control.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_processing.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/stream_line_source.hpp>
#include <loglib/tailing_bytes_producer.hpp>

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

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow)
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

    // Two-stage proxy chain: the inner `RowOrderProxyModel` reverses
    // row order in newest-first mode via O(1) per-row index mirroring;
    // the outer `LogFilterModel` filters and supports user-clicked
    // column sorts (sort role: `SortRole`). The layers never share sort
    // state, so a column sort and newest-first coexist without fighting.
    mRowOrderProxyModel = new RowOrderProxyModel(this);
    mRowOrderProxyModel->setSourceModel(mModel);

    mSortFilterProxyModel = new LogFilterModel(this);
    mSortFilterProxyModel->setSourceModel(mRowOrderProxyModel);
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
    connect(ui->actionOpenLogStream, &QAction::triggered, this, &MainWindow::OpenLogStream);
    connect(ui->actionSaveConfiguration, &QAction::triggered, this, &MainWindow::SaveConfiguration);
    connect(ui->actionLoadConfiguration, &QAction::triggered, this, &MainWindow::LoadConfiguration);
    connect(ui->actionExit, &QAction::triggered, this, &MainWindow::close);

    connect(ui->actionCopy, &QAction::triggered, mTableView, &LogTableView::CopySelectedRowsToClipboard);
    connect(ui->actionFind, &QAction::triggered, this, &MainWindow::Find);

    connect(ui->actionAddFilter, &QAction::triggered, this, [this]() { AddFilter(QUuid::createUuid().toString()); });
    connect(ui->actionClearAllFilters, &QAction::triggered, this, &MainWindow::ClearAllFilters);
    ui->actionClearAllFilters->setDisabled(true);

    // Stream toolbar. Hidden until a live-tail stream is opened; the
    // same actions are reachable from the Stream menu.
    mStreamToolbar = addToolBar(tr("Stream"));
    mStreamToolbar->setObjectName("streamToolbar");
    mStreamToolbar->addAction(ui->actionPauseStream);
    mStreamToolbar->addAction(ui->actionFollowTail);
    mStreamToolbar->addAction(ui->actionStopStream);
    mStreamToolbar->setVisible(false);

    // Disable the actions while idle so an idle-time Stream-menu click
    // on `actionPauseStream` (checkable) cannot leak its checked state
    // into the next session. `UpdateStreamToolbarVisibility` keeps the
    // enabled flag in sync with toolbar visibility from then on.
    ui->actionPauseStream->setEnabled(false);
    ui->actionFollowTail->setEnabled(false);
    ui->actionStopStream->setEnabled(false);

    connect(ui->actionPauseStream, &QAction::toggled, this, &MainWindow::TogglePauseStream);
    connect(ui->actionStopStream, &QAction::triggered, this, &MainWindow::StopStream);

    // `actionFollowTail` is a stateless toggle observed by
    // `ScrollToNewestRowIfFollowing`; the user-scroll signals below
    // auto-disengage / auto-re-engage it. The tail edge (bottom by
    // default, top in newest-first mode) is owned by
    // `ApplyDisplayOrder`, so this wiring stays
    // orientation-agnostic.
    //
    // Re-engage is gated on `IsLiveTailSession()` (not
    // `mModel->IsStreamingActive()`): static sessions are
    // streaming-active too while their backing file is parsed, but
    // they have no continuously-arriving "newest" data the user could
    // sensibly want to follow. Allowing a static-mode scroll to the
    // bottom to silently re-arm Follow newest would then yank the
    // viewport away on the next batch parsed from the same file.
    connect(mTableView, &LogTableView::userScrolledAwayFromTail, this, [this]() {
        if (ui->actionFollowTail->isChecked())
        {
            ui->actionFollowTail->setChecked(false);
        }
    });
    connect(mTableView, &LogTableView::userScrolledToTail, this, [this]() {
        if (!ui->actionFollowTail->isChecked() && IsLiveTailSession())
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
    // Both signals route through the same mode-aware slot. The slot
    // re-reads the per-mode `StreamingControl` accessor matching the
    // current session, so a stream-mode toggle is a no-op when a
    // static session is active and vice versa.
    connect(mPreferencesEditor, &PreferencesEditor::streamingDisplayOrderChanged, this, [this](bool) {
        ApplyDisplayOrder();
    });
    connect(mPreferencesEditor, &PreferencesEditor::staticDisplayOrderChanged, this, [this](bool) {
        ApplyDisplayOrder();
    });

    mStatusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(mStatusLabel);
    mStatusLabel->hide();

    connect(mModel, &LogModel::lineCountChanged, this, [this](qsizetype count) {
        mStreamingLineCount = count;
        UpdateStreamingStatus();
        // One-shot column auto-resize on the first non-empty batch so
        // initial widths fit the pre-fill rows; subsequent batches
        // leave columns alone to avoid yanking them under the mouse.
        if (IsLiveTailSession() && !mFirstStreamingBatchSeen && count > 0)
        {
            mFirstStreamingBatchSeen = true;
            UpdateUi();
        }
        // Auto-follow is a live-tail affordance only. Static sessions
        // are streaming-active too while the backing file is parsed,
        // but their "newest" row is just the next chunk of an
        // already-finite payload the user is reading -- yanking the
        // viewport on every batch fights the user's scroll.
        if (IsLiveTailSession())
        {
            ScrollToNewestRowIfFollowing();
        }
    });
    connect(mModel, &LogModel::errorCountChanged, this, [this](qsizetype count) {
        mStreamingErrorCount = count;
        UpdateStreamingStatus();
    });
    connect(mModel, &LogModel::streamingFinished, this, [this](StreamingResult result) {
        // Drop the `Source unavailable` latch so the post-session label
        // doesn't inherit a stale "waiting" state. The next stream's
        // first `Waiting` transition will re-set it.
        mSourceWaiting = false;

        // Multi-file static open: Success advances the queue;
        // Cancelled / Failed drain it so the user can react.
        //
        // `mSessionMode` stays at its pre-finish value (`Static`) across
        // the dispatch below so `StreamNextPendingFile` correctly treats
        // the next file as a continuation -- `isFirstFileInSession`
        // there reads `!IsSessionActive()`, so clearing the mode early
        // would route every follow-up file through `BeginStreaming`
        // (which resets the model) instead of `AppendStreaming`,
        // silently discarding the previously-parsed rows.
        if (result == StreamingResult::Success && !mPendingOpenFiles.isEmpty())
        {
            StreamNextPendingFile();
            if (mModel->IsStreamingActive())
            {
                return;
            }
            // Fallthrough: every remaining queued file failed to open.
            // The session is truly over; drop to the cleanup below.
        }
        else if (!mPendingOpenFiles.isEmpty())
        {
            mPendingOpenFiles.clear();
        }

        mSessionMode = SessionMode::Idle;

        // Reset Pause / Follow-tail to their defaults so the next
        // session starts unpaused with auto-scroll engaged. Neither
        // toggle has a `toggled` slot wired beyond the value
        // `ScrollToNewestRowIfFollowing` reads.
        if (ui->actionPauseStream->isChecked())
        {
            const QSignalBlocker blocker(ui->actionPauseStream);
            ui->actionPauseStream->setChecked(false);
        }
        if (!ui->actionFollowTail->isChecked())
        {
            const QSignalBlocker blocker(ui->actionFollowTail);
            ui->actionFollowTail->setChecked(true);
        }
        SetConfigurationUiEnabled(true);
        UpdateStreamToolbarVisibility();
        UpdateUi();
        UpdateStreamingStatus();
        // Only Success produces a post-parse error summary.
        if (result == StreamingResult::Success)
        {
            std::vector<std::string> errors = mModel->StreamingErrors();
            errors.insert(
                errors.end(),
                std::make_move_iterator(mPendingOpenErrors.begin()),
                std::make_move_iterator(mPendingOpenErrors.end())
            );
            mPendingOpenErrors.clear();
            ShowParseErrors("Error Parsing Logs", errors);
        }
        else
        {
            mPendingOpenErrors.clear();
        }
        mStreamingFileName.clear();
    });
    connect(mModel, &LogModel::rotationDetected, this, &MainWindow::OnRotationDetected);
    connect(mModel, &LogModel::sourceStatusChanged, this, &MainWindow::OnSourceStatusChanged);

    // Pull persisted streaming preferences into the model and proxy on
    // startup so the first session honours the user's choices.
    StreamingControl::LoadConfiguration();
    ApplyStreamingRetention();
    ApplyDisplayOrder();

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

    // Single-file drop preserves the "drop a config file to load it"
    // affordance; multi-file drops always stream.
    if (urlList.size() == 1)
    {
        const QString singleFile = urlList.front().toLocalFile();
        if (TryLoadAsConfiguration(singleFile))
        {
            UpdateUi();
            event->acceptProposedAction();
            return;
        }
    }

    QStringList files;
    files.reserve(urlList.size());
    for (const QUrl &url : urlList)
    {
        files.append(url.toLocalFile());
    }
    StartStreamingOpenQueue(std::move(files));

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
    case QEvent::ApplicationFontChange:
    {
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
    const QStringList files = QFileDialog::getOpenFileNames(this, "Select Log Files", QString(), "All Files (*.*)");
    if (files.isEmpty())
    {
        return;
    }

    // Single-file open preserves the "drop a config file to load it"
    // affordance; multi-file selections always stream.
    if (files.size() == 1 && TryLoadAsConfiguration(files.front()))
    {
        UpdateUi();
        return;
    }

    StartStreamingOpenQueue(files);
}

bool MainWindow::TryLoadAsConfiguration(const QString &file)
{
    try
    {
        mModel->ConfigurationManager().Load(file.toStdString());
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void MainWindow::StartStreamingOpenQueue(QStringList files)
{
    // Reset session state before starting so residual rows / filters /
    // paused buffer cannot leak into the new session.
    mModel->Reset();
    ClearAllFilters();

    mPendingOpenFiles = std::move(files);
    mPendingOpenErrors.clear();

    StreamNextPendingFile();
}

void MainWindow::StreamNextPendingFile()
{
    while (!mPendingOpenFiles.isEmpty())
    {
        const QString file = mPendingOpenFiles.takeFirst();

        // Open on the GUI thread so errors are synchronous. Failures
        // are accumulated; the queue continues with the next file.
        std::unique_ptr<loglib::LogFile> logFile;
        try
        {
            logFile = std::make_unique<loglib::LogFile>(file.toStdString());
        }
        catch (const std::exception &e)
        {
            mPendingOpenErrors.push_back(std::string("Failed to open '") + file.toStdString() + "': " + e.what());
            continue;
        }

        // Snapshot the configuration so the worker reads it lock-free.
        auto cfg = std::make_shared<const loglib::LogConfiguration>(mModel->Configuration());

        // First file in a session vs. follow-up: BeginStreaming starts
        // a new session; AppendStreaming joins the existing one.
        const bool isFirstFileInSession = !IsSessionActive();

        mStreamingFileName = QFileInfo(file).fileName();
        if (isFirstFileInSession)
        {
            mSessionMode = SessionMode::Static;
            mStreamingLineCount = 0;
            mStreamingErrorCount = 0;
            mFirstStreamingBatchSeen = false;
            SetConfigurationUiEnabled(false);
            UpdateStreamToolbarVisibility();
            // Re-apply the display-order so the proxy/view orientation
            // matches the static-mode preference before any rows arrive.
            ApplyDisplayOrder();
        }
        UpdateStreamingStatus();

        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(logFile));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        QtStreamingLogSink *sink = mModel->Sink();

        loglib::ParserOptions options;
        options.configuration = std::move(cfg);

        auto parseCallable = [sink, fileSourcePtr, options = std::move(options)](loglib::StopToken stopToken) mutable {
            options.stopToken = stopToken;
            loglib::JsonParser parser;
            parser.ParseStreaming(*fileSourcePtr, *sink, options);
        };

        if (isFirstFileInSession)
        {
            mModel->BeginStreaming(std::move(fileSource), std::move(parseCallable));
        }
        else
        {
            mModel->AppendStreaming(std::move(fileSource), std::move(parseCallable));
        }
        return;
    }

    // Queue exhausted via fallthrough (every remaining file failed to
    // open). Surface the accumulated errors now if no session was ever
    // armed; otherwise the `streamingFinished` summary will fold them in.
    if (!IsSessionActive() && !mPendingOpenErrors.empty())
    {
        ShowParseErrors("Error Opening File", mPendingOpenErrors);
        mPendingOpenErrors.clear();
    }
}

void MainWindow::OpenLogStream()
{
    const QString file = QFileDialog::getOpenFileName(this, "Open Log Stream...", QString(), "All Files (*.*)");
    if (file.isEmpty())
    {
        return;
    }

    // Construct the source on the GUI thread so open errors are
    // synchronous. Pre-fill back-reads at most `retention` lines.
    const size_t retention =
        (mModel->RetentionCap() != 0) ? mModel->RetentionCap() : StreamingControl::RetentionLines();

    std::filesystem::path filePath(file.toStdString());
    std::unique_ptr<loglib::TailingBytesProducer> source;
    try
    {
        source = std::make_unique<loglib::TailingBytesProducer>(filePath, retention);
    }
    catch (const std::exception &e)
    {
        ShowParseErrors(
            "Error Opening Log Stream",
            {std::string("Failed to open '") + file.toStdString() + "' for streaming: " + e.what()}
        );
        return;
    }

    // Mirror the static-open reset order so residual state (filters,
    // Find match, paused buffer) cannot leak into the new session.
    mModel->Reset();
    ClearAllFilters();

    mStreamingFileName = QFileInfo(file).fileName();
    mSessionMode = SessionMode::LiveTail;
    mStreamingLineCount = 0;
    mStreamingErrorCount = 0;
    mFirstStreamingBatchSeen = false;
    SetConfigurationUiEnabled(false);
    UpdateStreamingStatus();
    UpdateStreamToolbarVisibility();
    // Re-apply the display-order so the proxy/view orientation matches
    // the stream-mode preference before any rows arrive.
    ApplyDisplayOrder();

    // Snapshot the configuration so the worker reads it lock-free.
    auto cfg = std::make_shared<const loglib::LogConfiguration>(mModel->Configuration());

    loglib::ParserOptions options;
    options.configuration = std::move(cfg);

    // Wrap the producer in a long-lived `StreamLineSource`: the model
    // installs it in `LogTable` and the parser tags each `LogLine`
    // with it so `LineSource::RawLine` resolves the bytes later.
    auto streamSource = std::make_unique<loglib::StreamLineSource>(filePath, std::move(source));
    mModel->BeginStreaming(std::move(streamSource), std::move(options));
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
    // `StopAndKeepRows()` runs the teardown sequence and emits a
    // compensating `streamingFinished(Cancelled)`, but preserves the
    // visible rows so the user can keep working on them.
    mModel->StopAndKeepRows();
    mStreamingFileName.clear();
}

void MainWindow::OnRotationDetected()
{
    // Brief 3 s `— rotated` flash on the status label.
    mRotationFlashActive = true;
    UpdateStreamingStatus();
    QTimer::singleShot(3000, this, [this]() {
        mRotationFlashActive = false;
        UpdateStreamingStatus();
    });
}

void MainWindow::OnSourceStatusChanged(loglib::SourceStatus status)
{
    // Latch the `Waiting` state so the label keeps showing
    // "Source unavailable …" between callbacks (which are edge-triggered).
    mSourceWaiting = (status == loglib::SourceStatus::Waiting);
    UpdateStreamingStatus();
}

void MainWindow::SetConfigurationUiEnabled(bool enabled)
{
    // Parser holds an immutable snapshot; gate edits while streaming.
    ui->actionLoadConfiguration->setEnabled(enabled);
    ui->actionSaveConfiguration->setEnabled(enabled);
    ui->actionPreferences->setEnabled(enabled);
}

void MainWindow::UpdateStreamingStatus()
{
    if (!IsSessionActive())
    {
        mStatusLabel->clear();
        mStatusLabel->hide();
        return;
    }

    QString text;
    if (!IsLiveTailSession())
    {
        text = QString("Parsing %1 - %2 lines, %3 errors")
                   .arg(mStreamingFileName)
                   .arg(mStreamingLineCount)
                   .arg(mStreamingErrorCount);
    }
    else if (mSourceWaiting)
    {
        // Source unavailable takes precedence over Paused: it surfaces
        // an environmental problem the user should react to.
        text = QString("Source unavailable - last seen %1 - %2 lines, %3 errors")
                   .arg(mStreamingFileName)
                   .arg(mStreamingLineCount)
                   .arg(mStreamingErrorCount);
    }
    else if (mModel->Sink() && mModel->Sink()->IsPaused())
    {
        const auto buffered = static_cast<qsizetype>(mModel->Sink()->PausedLineCount());
        text = QString("Paused - %1 lines, %2 buffered").arg(mStreamingLineCount).arg(buffered);
    }
    else
    {
        text = QString("Streaming %1 - %2 lines, %3 errors")
                   .arg(mStreamingFileName)
                   .arg(mStreamingLineCount)
                   .arg(mStreamingErrorCount);
    }

    // Paused-drop telemetry: stays non-zero until the stream is
    // stopped so the user keeps seeing "lines were lost" after Resume.
    // Static-streaming sessions do not pause in practice.
    if (IsLiveTailSession() && mModel->Sink())
    {
        const auto dropped = static_cast<qsizetype>(mModel->Sink()->PausedDropCount());
        if (dropped > 0)
        {
            text += QString(", %1 dropped while paused").arg(dropped);
        }
    }

    if (IsLiveTailSession() && mRotationFlashActive)
    {
        text += " - rotated";
    }

    mStatusLabel->setText(text);
    mStatusLabel->show();
}

void MainWindow::UpdateStreamToolbarVisibility()
{
    // Use this window's `mSessionMode` rather than
    // `mModel->IsStreamingActive()`: the model flag is only set
    // inside `BeginStreaming`, which runs after this is called from
    // open paths.
    const bool visible = IsLiveTailSession();
    if (mStreamToolbar)
    {
        mStreamToolbar->setVisible(visible);
    }
    // Gate the menu actions on toolbar visibility so a Stream-menu
    // click while idle cannot pre-flip a checkable action's state into
    // the next session. Per-session checked-state reset lives in the
    // `streamingFinished` slot above.
    ui->actionPauseStream->setEnabled(visible);
    ui->actionFollowTail->setEnabled(visible);
    ui->actionStopStream->setEnabled(visible);
}

void MainWindow::ScrollToNewestRowIfFollowing()
{
    // Auto-follow is meaningful only in live-tail sessions; static
    // sessions parse a finite payload the user is reading, so any
    // residual `actionFollowTail` checked state from the previous
    // session (or the startup default) must not cause the viewport
    // to chase batches as the file is parsed. The toolbar gate in
    // `UpdateStreamToolbarVisibility` already disables the action
    // outside live-tail, but the action's *value* is independent of
    // its enabled flag so we need this defensive early-return too.
    if (!IsLiveTailSession())
    {
        return;
    }
    if (!ui->actionFollowTail->isChecked())
    {
        return;
    }
    const int sourceRowCount = mModel->rowCount();
    if (sourceRowCount <= 0)
    {
        return;
    }
    // Scroll to the most-recently-appended source row even when sorted
    // by a non-time column; mapping through both proxy layers makes
    // the scroll land on the correct visual row under sort / filter /
    // reverse-order.
    const QModelIndex sourceIndex = mModel->index(sourceRowCount - 1, 0);
    const QModelIndex midIndex = mRowOrderProxyModel->mapFromSource(sourceIndex);
    const QModelIndex proxyIndex = mSortFilterProxyModel->mapFromSource(midIndex);
    if (!proxyIndex.isValid())
    {
        return;
    }
    // In newest-first mode the latest row is at proxy row 0, so we
    // must scroll to the top edge. The view's `TailEdge` is kept in
    // sync by `ApplyDisplayOrder`.
    const auto position =
        mRowOrderProxyModel->IsReversed() ? QAbstractItemView::PositionAtTop : QAbstractItemView::PositionAtBottom;
    mTableView->scrollTo(proxyIndex, position);
}

void MainWindow::ApplyStreamingRetention()
{
    mModel->SetRetentionCap(StreamingControl::RetentionLines());
}

QAction *MainWindow::FindUiAction(const QString &name) const
{
    // Exhaustive dispatch against `ui->` so tests can reach every
    // UI-file-declared action without depending on the QObject tree.
    // Must stay in sync with `main_window.ui`.
    if (name == QStringLiteral("actionOpen"))
    {
        return ui->actionOpen;
    }
    if (name == QStringLiteral("actionOpenLogStream"))
    {
        return ui->actionOpenLogStream;
    }
    if (name == QStringLiteral("actionSaveConfiguration"))
    {
        return ui->actionSaveConfiguration;
    }
    if (name == QStringLiteral("actionLoadConfiguration"))
    {
        return ui->actionLoadConfiguration;
    }
    if (name == QStringLiteral("actionExit"))
    {
        return ui->actionExit;
    }
    if (name == QStringLiteral("actionCopy"))
    {
        return ui->actionCopy;
    }
    if (name == QStringLiteral("actionFind"))
    {
        return ui->actionFind;
    }
    if (name == QStringLiteral("actionAddFilter"))
    {
        return ui->actionAddFilter;
    }
    if (name == QStringLiteral("actionClearAllFilters"))
    {
        return ui->actionClearAllFilters;
    }
    if (name == QStringLiteral("actionPauseStream"))
    {
        return ui->actionPauseStream;
    }
    if (name == QStringLiteral("actionFollowTail"))
    {
        return ui->actionFollowTail;
    }
    if (name == QStringLiteral("actionStopStream"))
    {
        return ui->actionStopStream;
    }
    if (name == QStringLiteral("actionPreferences"))
    {
        return ui->actionPreferences;
    }
    return nullptr;
}

void MainWindow::SetSessionModeForTest(TestSessionMode mode)
{
    switch (mode)
    {
    case TestSessionMode::Idle:
        mSessionMode = SessionMode::Idle;
        break;
    case TestSessionMode::Static:
        mSessionMode = SessionMode::Static;
        break;
    case TestSessionMode::LiveTail:
        mSessionMode = SessionMode::LiveTail;
        break;
    }
}

void MainWindow::ApplyDisplayOrder()
{
    // Mode dispatch:
    //   - LiveTail session  -> stream-mode preference
    //   - Static session    -> static-mode preference
    //   - Idle              -> stream-mode preference (the *next*
    //     session typically inherits this on open; whichever path
    //     opens the file calls `ApplyDisplayOrder()` again with the
    //     correct `mSessionMode` so this fallback only matters for
    //     the empty viewer between sessions).
    const bool newestFirst = (mSessionMode == SessionMode::Static) ? StreamingControl::IsStaticNewestFirst()
                                                                   : StreamingControl::IsNewestFirst();

    mRowOrderProxyModel->SetReversed(newestFirst);

    mTableView->SetTailEdge(newestFirst ? LogTableView::TailEdge::Top : LogTableView::TailEdge::Bottom);

    // Disable alternating row colours in newest-first mode. Qt keys
    // alternation off the visual row index, so top-insertion flips
    // every visible row's parity on every batch and the rows flicker.
    // A custom delegate could pin colours to source-row parity, but
    // the CSS-based `alternate-background-color` bypasses it.
    mTableView->setAlternatingRowColors(!newestFirst);

    // Re-pin to the new tail edge if Follow tail is engaged so the
    // user does not have to scroll manually after toggling the option.
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

    constexpr size_t MAX_ERRORS_SHOWN = 20;
    QString message;
    const size_t shown = std::min(errors.size(), MAX_ERRORS_SHOWN);
    for (size_t i = 0; i < shown; ++i)
    {
        message += QString::fromStdString(errors[i]) + QLatin1Char('\n');
    }
    if (errors.size() > MAX_ERRORS_SHOWN)
    {
        message += QString("... and %1 more error(s).").arg(errors.size() - MAX_ERRORS_SHOWN);
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
            mModel->Reset();
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
