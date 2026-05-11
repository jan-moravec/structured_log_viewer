#include "main_window.hpp"
#include "./ui_main_window.h"

#include "appearance_control.hpp"
#include "filter_editor.hpp"
#include "network_stream_dialog.hpp"
#include "qt_streaming_log_sink.hpp"
#include "streaming_control.hpp"

#include <loglib/bytes_producer.hpp>
#include <loglib/enum_dictionary.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_processing.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/stream_line_source.hpp>
#include <loglib/tailing_bytes_producer.hpp>
#include <loglib/tcp_server_producer.hpp>
#include <loglib/udp_server_producer.hpp>

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

// Locate the staged `tzdata/` directory. Tries (in order) the binary
// directory, the macOS Resources bundle, $APPDIR/usr/share/tzdata, then
// the CWD ancestor chain. Empty path on miss; `searched` accumulates
// candidates for diagnostics.
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

// How long transient status-bar messages (filter rejection / drop notices)
// linger before the bar reverts to default state.
constexpr int STATUS_BAR_MESSAGE_TIMEOUT_MS = 5000;

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

    // Proxy chain: `RowOrderProxyModel` mirrors row indices for
    // newest-first; `LogFilterModel` handles filtering and column
    // sorts. They never share sort state.
    mRowOrderProxyModel = new RowOrderProxyModel(this);
    mRowOrderProxyModel->setSourceModel(mModel);

    mSortFilterProxyModel = new LogFilterModel(this);
    mSortFilterProxyModel->setSourceModel(mRowOrderProxyModel);
    mSortFilterProxyModel->SetLogModel(mModel);
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
    connect(ui->actionOpenNetworkStream, &QAction::triggered, this, &MainWindow::OpenNetworkStream);
    connect(ui->actionSaveConfiguration, &QAction::triggered, this, &MainWindow::SaveConfiguration);
    connect(ui->actionLoadConfiguration, &QAction::triggered, this, &MainWindow::LoadConfiguration);
    connect(ui->actionExit, &QAction::triggered, this, &MainWindow::close);

    connect(ui->actionCopy, &QAction::triggered, mTableView, &LogTableView::CopySelectedRowsToClipboard);
    connect(ui->actionFind, &QAction::triggered, this, &MainWindow::Find);

    connect(ui->actionAddFilter, &QAction::triggered, this, [this]() { AddFilter(QUuid::createUuid().toString()); });
    connect(ui->actionClearAllFilters, &QAction::triggered, this, &MainWindow::ClearAllFilters);
    ui->actionClearAllFilters->setDisabled(true);

    // Stream toolbar; hidden until a live-tail stream is opened. The
    // same actions are also in the Stream menu.
    mStreamToolbar = addToolBar(tr("Stream"));
    mStreamToolbar->setObjectName("streamToolbar");
    mStreamToolbar->addAction(ui->actionPauseStream);
    mStreamToolbar->addAction(ui->actionFollowTail);
    mStreamToolbar->addAction(ui->actionStopStream);
    mStreamToolbar->setVisible(false);

    // Disable while idle so a checked state cannot leak into the next
    // session; `UpdateStreamToolbarVisibility` keeps these in sync after.
    ui->actionPauseStream->setEnabled(false);
    ui->actionFollowTail->setEnabled(false);
    ui->actionStopStream->setEnabled(false);

    connect(ui->actionPauseStream, &QAction::toggled, this, &MainWindow::TogglePauseStream);
    connect(ui->actionStopStream, &QAction::triggered, this, &MainWindow::StopStream);

    // `actionFollowTail` is auto-disengaged when the user scrolls away
    // and auto-re-engaged on tail-edge scroll. Re-engage is gated on
    // live-tail sessions only; static sessions don't follow.
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
    // Mode-aware slot reads the per-mode `StreamingControl` accessor;
    // off-mode toggles are no-ops.
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
        // One-shot column auto-resize on the first non-empty batch.
        if (IsLiveTailSession() && !mFirstStreamingBatchSeen && count > 0)
        {
            mFirstStreamingBatchSeen = true;
            UpdateUi();
        }
        // Auto-follow is live-tail only.
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
        // Clear the `Source unavailable` latch.
        mSourceWaiting = false;

        // Multi-file static open: Success advances the queue. Keep
        // `mSessionMode == Static` across `StreamNextPendingFile` so
        // it routes follow-up files through `AppendStreaming`.
        if (result == StreamingResult::Success && !mPendingOpenFiles.isEmpty())
        {
            StreamNextPendingFile();
            if (mModel->IsStreamingActive())
            {
                return;
            }
            // Fallthrough: queue drained without a new active session.
        }
        else if (!mPendingOpenFiles.isEmpty())
        {
            mPendingOpenFiles.clear();
        }

        mSessionMode = SessionMode::Idle;

        // Reset Pause / Follow-tail to defaults for the next session.
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
    // Keep enum filter bitsets and sort ranks in sync with the live
    // dictionary. Always drop cached ranks; rebuild predicates only
    // when an active enum filter has unresolved selected values
    // (skips growth that only minted unselected ids).
    connect(mModel, &LogModel::enumColumnsChanged, this, [this]() {
        mSortFilterProxyModel->InvalidateEnumRanks();
        const bool anyUnresolved = std::ranges::any_of(mFilters, [this](const auto &kv) {
            return kv.second.type == loglib::LogConfiguration::LogFilter::Type::Enumeration &&
                   !EnumFilterFullyResolved(kv.second);
        });
        if (anyUnresolved)
        {
            UpdateFilters();
        }
    });

    // Pull persisted streaming preferences on startup.
    StreamingControl::LoadConfiguration();
    ApplyStreamingRetention();
    ApplyDisplayOrder();

    QTimer::singleShot(0, [] {
        // qCritical instead of a modal: offscreen Qt hangs on modals.
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

    // Single-file drop may load as a configuration; multi-file always streams.
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

    // Single-file open may load as a configuration; multi-file always streams.
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
    // Reset before starting so residual state cannot leak in.
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

        // Open on the GUI thread so errors are synchronous; queue
        // continues with the next file on failure.
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

        // BeginStreaming starts a session; AppendStreaming continues it.
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
            // Re-apply display-order so view orientation matches the
            // static-mode preference before any rows arrive.
            ApplyDisplayOrder();
        }
        UpdateStreamingStatus();

        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(logFile));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        QtStreamingLogSink *sink = mModel->Sink();

        loglib::ParserOptions options;
        options.configuration = std::move(cfg);

        // False positive: `parseCallable` is moved into the model and invoked;
        // `cfg` is consumed by `options`.
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
        auto parseCallable = [sink, fileSourcePtr, options = std::move(options)](const loglib::StopToken &stopToken
                             ) mutable {
            options.stopToken = stopToken;
            const loglib::JsonParser parser;
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

    // Queue drained without a session ever arming: surface errors now;
    // otherwise the `streamingFinished` summary folds them in.
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

    // Construct on the GUI thread for synchronous open errors.
    const size_t retention =
        (mModel->RetentionCap() != 0) ? mModel->RetentionCap() : StreamingControl::RetentionLines();

    const std::filesystem::path filePath(file.toStdString());
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

    // Mirror the static-open reset so residual state cannot leak in.
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
    ApplyDisplayOrder();

    auto cfg = std::make_shared<const loglib::LogConfiguration>(mModel->Configuration());

    loglib::ParserOptions options;
    options.configuration = std::move(cfg);

    // Wrap the producer in a `StreamLineSource` so each `LogLine` can
    // resolve its bytes via `LineSource::RawLine` later.
    auto streamSource = std::make_unique<loglib::StreamLineSource>(filePath, std::move(source));
    mModel->BeginStreaming(std::move(streamSource), std::move(options));
}

void MainWindow::OpenNetworkStream()
{
    NetworkStreamDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    const auto cfg = dialog.Configuration();

    std::unique_ptr<loglib::BytesProducer> producer;
    std::string displayName;
    try
    {
        if (cfg.protocol == NetworkStreamDialog::Protocol::Tcp)
        {
            loglib::TcpServerProducer::Options opts;
            opts.bindAddress = cfg.bindAddress.toStdString();
            opts.port = cfg.port;
            opts.maxConcurrentClients = cfg.maxConcurrentClients;
            if (cfg.tlsEnabled)
            {
                opts.tls.emplace();
                opts.tls->certificateChain = cfg.tlsCertChainPath.toStdString();
                opts.tls->privateKey = cfg.tlsPrivateKeyPath.toStdString();
                opts.tls->caBundle = cfg.tlsCaBundlePath.toStdString();
                opts.tls->requireClientCertificate = cfg.tlsRequireClientCertificate;
            }
            auto tcp = std::make_unique<loglib::TcpServerProducer>(std::move(opts));
            displayName = tcp->DisplayName();
            producer = std::move(tcp);
        }
        else
        {
            loglib::UdpServerProducer::Options opts;
            opts.bindAddress = cfg.bindAddress.toStdString();
            opts.port = cfg.port;
            auto udp = std::make_unique<loglib::UdpServerProducer>(std::move(opts));
            displayName = udp->DisplayName();
            producer = std::move(udp);
        }
    }
    catch (const std::exception &e)
    {
        ShowParseErrors(
            "Error Opening Network Stream",
            {std::string("Failed to start network listener on ") + cfg.bindAddress.toStdString() + ":" +
             std::to_string(cfg.port) + ": " + e.what()}
        );
        return;
    }

    // Mirror the OpenLogStream reset.
    mModel->Reset();
    ClearAllFilters();

    mStreamingFileName = QString::fromStdString(displayName);
    mSessionMode = SessionMode::LiveTail;
    mStreamingLineCount = 0;
    mStreamingErrorCount = 0;
    mFirstStreamingBatchSeen = false;
    SetConfigurationUiEnabled(false);
    UpdateStreamingStatus();
    UpdateStreamToolbarVisibility();
    ApplyDisplayOrder();

    auto config = std::make_shared<const loglib::LogConfiguration>(mModel->Configuration());
    loglib::ParserOptions options;
    options.configuration = std::move(config);

    // Network streams have no real filesystem path; the producer's
    // display string serves as the LineSource's opaque identity.
    auto streamSource =
        std::make_unique<loglib::StreamLineSource>(std::filesystem::path(displayName), std::move(producer));
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
    // Tear down but keep visible rows so the user can keep working on them.
    mModel->StopAndKeepRows();
    mStreamingFileName.clear();
}

void MainWindow::OnRotationDetected()
{
    constexpr int ROTATION_STATUS_FLASH_MS = 3000;
    mRotationFlashActive = true;
    UpdateStreamingStatus();
    QTimer::singleShot(ROTATION_STATUS_FLASH_MS, this, [this]() {
        mRotationFlashActive = false;
        UpdateStreamingStatus();
    });
}

void MainWindow::OnSourceStatusChanged(loglib::SourceStatus status)
{
    // Latch `Waiting` so the label keeps showing "Source unavailable".
    mSourceWaiting = (status == loglib::SourceStatus::Waiting);
    UpdateStreamingStatus();
}

void MainWindow::SetConfigurationUiEnabled(bool enabled)
{
    // Parser snapshot is immutable; gate config edits while streaming.
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
        // Source unavailable takes precedence over Paused.
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

    // Paused-drop telemetry stays non-zero across Resume so the user
    // keeps seeing "lines were lost" until Stop.
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
    // Read `mSessionMode` (set on the open path) rather than the model
    // flag (set later inside `BeginStreaming`).
    const bool visible = IsLiveTailSession();
    if (mStreamToolbar)
    {
        mStreamToolbar->setVisible(visible);
    }
    // Gate menu actions so an idle click cannot pre-flip a checkable
    // action's state into the next session.
    ui->actionPauseStream->setEnabled(visible);
    ui->actionFollowTail->setEnabled(visible);
    ui->actionStopStream->setEnabled(visible);
}

void MainWindow::ScrollToNewestRowIfFollowing()
{
    // Auto-follow is live-tail only; defensive against stale
    // `actionFollowTail` value (the action's checked state is
    // independent of its enabled flag).
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
    // Map through both proxy layers so the scroll lands on the correct
    // visual row under sort / filter / reverse-order.
    const QModelIndex sourceIndex = mModel->index(sourceRowCount - 1, 0);
    const QModelIndex midIndex = mRowOrderProxyModel->mapFromSource(sourceIndex);
    const QModelIndex proxyIndex = mSortFilterProxyModel->mapFromSource(midIndex);
    if (!proxyIndex.isValid())
    {
        return;
    }
    // Newest-first puts the latest row at proxy row 0; tail edge is
    // owned by `ApplyDisplayOrder`.
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
    // Must stay in sync with `main_window.ui`.
    if (name == QStringLiteral("actionOpen"))
    {
        return ui->actionOpen;
    }
    if (name == QStringLiteral("actionOpenLogStream"))
    {
        return ui->actionOpenLogStream;
    }
    if (name == QStringLiteral("actionOpenNetworkStream"))
    {
        return ui->actionOpenNetworkStream;
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

QMenu *MainWindow::FiltersMenu() const
{
    return ui->menuFilters;
}

#ifdef LOGAPP_BUILD_TESTING
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
#endif

void MainWindow::ApplyDisplayOrder()
{
    // Static -> static-mode preference; everything else -> stream-mode.
    const bool newestFirst = (mSessionMode == SessionMode::Static) ? StreamingControl::IsStaticNewestFirst()
                                                                   : StreamingControl::IsNewestFirst();

    mRowOrderProxyModel->SetReversed(newestFirst);

    mTableView->SetTailEdge(newestFirst ? LogTableView::TailEdge::Top : LogTableView::TailEdge::Bottom);

    // Newest-first disables alternating colours: Qt keys alternation off
    // the visual row index, so top-insertion would flicker every row.
    mTableView->setAlternatingRowColors(!newestFirst);

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
    const QString file =
        QFileDialog::getSaveFileName(this, "Save Configuration", QString(), "JSON (*.json);;All Files (*)");
    if (!file.isEmpty())
    {
        mModel->ConfigurationManager().Save(file.toStdString());
    }
}

void MainWindow::LoadConfiguration()
{
    const QString file =
        QFileDialog::getOpenFileName(this, "Load Configuration", QString(), "JSON (*.json);;All Files (*)");
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
    // `searchStartIndex` is already in proxy coords (it came from
    // `mTableView->currentIndex()`). Pass it through directly so
    // `MatchRow` never sees a mixed coordinate space.
    QModelIndexList matches =
        mSortFilterProxyModel->MatchRow(searchStartIndex, Qt::DisplayRole, value, 1, flags, next, skipFirstN);

    if (!matches.isEmpty())
    {
        mTableView->clearSelection();
        mTableView->scrollTo(matches[0]);
        mTableView->selectionModel()->select(matches[0], QItemSelectionModel::Select | QItemSelectionModel::Rows);
        mTableView->selectionModel()->setCurrentIndex(matches[0], QItemSelectionModel::NoUpdate);
    }
}

void MainWindow::AddFilter(
    const QString &filterId, const std::optional<loglib::LogConfiguration::LogFilter> &filter, bool openEditor
)
{
    if (mModel->rowCount() == 0)
    {
        return;
    }

    // Drop saved filters whose type no longer matches the column type
    // (e.g. string filter against a column that auto-promoted to enum).
    std::optional<loglib::LogConfiguration::LogFilter> resolvedFilter = filter;
    if (resolvedFilter.has_value())
    {
        const auto &columns = mModel->Configuration().columns;
        const auto rowIndex = static_cast<size_t>(resolvedFilter->row);
        // Saved empty enum selection: would hide every row.
        if (rowIndex < columns.size() &&
            resolvedFilter->type == loglib::LogConfiguration::LogFilter::Type::Enumeration &&
            resolvedFilter->filterValues.empty())
        {
            statusBar()->showMessage(
                QString("Saved enumeration filter for '%1' had no values selected; ignoring")
                    .arg(QString::fromStdString(columns[rowIndex].header)),
                STATUS_BAR_MESSAGE_TIMEOUT_MS
            );
            ClearFilter(filterId);
            return;
        }
        if (rowIndex < columns.size())
        {
            const loglib::LogConfiguration::Type columnType = columns[rowIndex].type;
            const loglib::LogConfiguration::LogFilter::Type filterType = resolvedFilter->type;

            const bool typesMatch = (filterType == loglib::LogConfiguration::LogFilter::Type::Time &&
                                     columnType == loglib::LogConfiguration::Type::Time) ||
                                    (filterType == loglib::LogConfiguration::LogFilter::Type::Enumeration &&
                                     columnType == loglib::LogConfiguration::Type::Enumeration) ||
                                    (filterType == loglib::LogConfiguration::LogFilter::Type::String &&
                                     columnType != loglib::LogConfiguration::Type::Time &&
                                     columnType != loglib::LogConfiguration::Type::Enumeration);
            if (!typesMatch)
            {
                ClearFilter(filterId);
                statusBar()->showMessage(
                    QString("Filter '%1' was removed because the column type changed")
                        .arg(QString::fromStdString(columns[rowIndex].header)),
                    STATUS_BAR_MESSAGE_TIMEOUT_MS
                );
                resolvedFilter.reset();
                if (!openEditor)
                {
                    return;
                }
                // Intentional fallthrough: the saved filter was dropped
                // because the column type changed; open a fresh editor
                // so the user can re-pick values for the new type.
                // Regression: `TestSavedStringFilterDroppedOnNowEnumColumn`.
            }
        }
    }

    if (!openEditor)
    {
        // Configuration-load path: filter is already in `mFilters`.
        return;
    }

    auto *filterEditor = new FilterEditor(*mModel, filterId, this);
    connect(filterEditor, &FilterEditor::FilterSubmitted, this, &MainWindow::FilterSubmitted);
    connect(filterEditor, &FilterEditor::FilterTimeStampSubmitted, this, &MainWindow::FilterTimeStampSubmitted);
    connect(filterEditor, &FilterEditor::FilterEnumSubmitted, this, &MainWindow::FilterEnumSubmitted);
    if (resolvedFilter.has_value())
    {
        if (resolvedFilter->type == loglib::LogConfiguration::LogFilter::Type::Time)
        {
            if (!resolvedFilter->filterBegin.has_value() || !resolvedFilter->filterEnd.has_value())
            {
                statusBar()->showMessage(
                    QString("Filter '%1' was dropped because its time range is missing").arg(filterId),
                    STATUS_BAR_MESSAGE_TIMEOUT_MS
                );
                ClearFilter(filterId);
                delete filterEditor;
                return;
            }
            filterEditor->Load(resolvedFilter->row, *resolvedFilter->filterBegin, *resolvedFilter->filterEnd);
        }
        else if (resolvedFilter->type == loglib::LogConfiguration::LogFilter::Type::Enumeration)
        {
            QStringList values;
            values.reserve(static_cast<qsizetype>(resolvedFilter->filterValues.size()));
            for (const std::string &v : resolvedFilter->filterValues)
            {
                values.append(QString::fromStdString(v));
            }
            filterEditor->Load(resolvedFilter->row, values);
        }
        else
        {
            if (!resolvedFilter->filterString.has_value() || !resolvedFilter->matchType.has_value())
            {
                statusBar()->showMessage(
                    QString("Filter '%1' was dropped because its string match is missing").arg(filterId),
                    STATUS_BAR_MESSAGE_TIMEOUT_MS
                );
                ClearFilter(filterId);
                delete filterEditor;
                return;
            }
            filterEditor->Load(
                resolvedFilter->row,
                QString::fromStdString(*resolvedFilter->filterString),
                static_cast<int>(*resolvedFilter->matchType)
            );
        }
    }
    filterEditor->show();
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
    const auto match = static_cast<loglib::LogConfiguration::LogFilter::Match>(matchType);

    // Reject an invalid regex up front; the downstream
    // `QRegularExpression` would otherwise compile to an invalid
    // object and silently hide every row. Wildcards always compile.
    if (match == loglib::LogConfiguration::LogFilter::Match::RegularExpression)
    {
        const QRegularExpression probe(filterString);
        if (!probe.isValid())
        {
            statusBar()->showMessage(
                QString("Invalid regular expression: %1").arg(probe.errorString()), STATUS_BAR_MESSAGE_TIMEOUT_MS
            );
            ClearFilter(filterID);
            return;
        }
    }

    ClearFilter(filterID);

    loglib::LogConfiguration::LogFilter filter;
    filter.type = loglib::LogConfiguration::LogFilter::Type::String;
    filter.row = row;
    filter.filterString = filterString.toStdString();
    filter.matchType = match;

    AddLogFilter(filterID, filter);
}

void MainWindow::FilterTimeStampSubmitted(const QString &filterID, int row, qint64 beginTimeStamp, qint64 endTimeStamp)
{
    // Reject an inverted range up front; the predicate would
    // otherwise hide every row silently. Mirrors the regex probe
    // in `FilterSubmitted`.
    if (beginTimeStamp > endTimeStamp)
    {
        statusBar()->showMessage(
            QString("Time-range filter rejected: begin (%1) is after end (%2)").arg(beginTimeStamp).arg(endTimeStamp),
            STATUS_BAR_MESSAGE_TIMEOUT_MS
        );
        ClearFilter(filterID);
        return;
    }

    ClearFilter(filterID);

    loglib::LogConfiguration::LogFilter filter;
    filter.type = loglib::LogConfiguration::LogFilter::Type::Time;
    filter.row = row;
    filter.filterBegin = beginTimeStamp;
    filter.filterEnd = endTimeStamp;

    AddLogFilter(filterID, filter);
}

void MainWindow::FilterEnumSubmitted(const QString &filterID, int row, const QStringList &selectedValues)
{
    ClearFilter(filterID);

    loglib::LogConfiguration::LogFilter filter;
    filter.type = loglib::LogConfiguration::LogFilter::Type::Enumeration;
    filter.row = row;
    filter.filterValues.reserve(static_cast<size_t>(selectedValues.size()));
    for (const QString &v : selectedValues)
    {
        filter.filterValues.push_back(v.toStdString());
    }

    AddLogFilter(filterID, filter);
}

void MainWindow::AddLogFilter(const QString &id, const loglib::LogConfiguration::LogFilter &filter)
{
    mFilters[id.toStdString()] = filter;
    UpdateFilters();

    QString title;
    switch (filter.type)
    {
    case loglib::LogConfiguration::LogFilter::Type::Time:
        title = QString::fromStdString(
            loglib::UtcMicrosecondsToDateTimeString(*filter.filterBegin) + " - " +
            loglib::UtcMicrosecondsToDateTimeString(*filter.filterEnd)
        );
        break;
    case loglib::LogConfiguration::LogFilter::Type::Enumeration:
    {
        QStringList values;
        values.reserve(static_cast<qsizetype>(filter.filterValues.size()));
        for (const std::string &v : filter.filterValues)
        {
            values.append(QString::fromStdString(v));
        }
        title = values.join(QStringLiteral(", "));
        break;
    }
    case loglib::LogConfiguration::LogFilter::Type::String:
    default:
        title = QString::fromStdString(*filter.filterString);
        break;
    }

    QMenu *menuItem = ui->menuFilters->addMenu(title);
    menuItem->menuAction()->setData(QVariant(id));

    const QAction *editAction = menuItem->addAction("Edit");
    // Qt slot; `AddFilter` uses normal exception-throwing STL; failures surface as usual.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    connect(editAction, &QAction::triggered, this, [this, id, filter]() { AddFilter(id, filter); });

    const QAction *clearAction = menuItem->addAction("Clear");
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

const loglib::EnumDictionary *MainWindow::ResolveEnumDictionary(int columnIndex) const
{
    if (columnIndex < 0)
    {
        return nullptr;
    }
    return mModel->Table().ResolveEnumColumn(static_cast<size_t>(columnIndex)).dictionary;
}

bool MainWindow::EnumFilterFullyResolved(const loglib::LogConfiguration::LogFilter &filter) const
{
    if (filter.type != loglib::LogConfiguration::LogFilter::Type::Enumeration)
    {
        return true;
    }
    const loglib::EnumDictionary *dictionary = ResolveEnumDictionary(filter.row);
    if (dictionary == nullptr)
    {
        // Column not yet promoted: defer resolution until first growth.
        return false;
    }
    return std::ranges::all_of(filter.filterValues, [dictionary](const std::string &value) {
        return dictionary->Find(value) != loglib::INVALID_ENUM_VALUE_ID;
    });
}

namespace
{

/// JIT-compile @p regex eagerly so each captured copy doesn't re-JIT
/// on first `match()` (would be a thread-safety footgun off the GUI).
void PrimeRegex(QRegularExpression &regex)
{
    (void)regex.match(QStringLiteral(""));
}

/// Build a Qt-flavoured matcher for `CallbackStringRowPredicate`.
/// Captures the compiled regex / needle once so the inner loop avoids
/// per-row recompiles.
///
/// The haystack is normalised via
/// `LogModel::ConvertToSingleLineCompactQString` so filters match the
/// same single-line text the user sees (and that Find applies). The
/// needle is left as-typed -- mirrors the pre-`RowPredicate`
/// `SortRole` semantics so a needle with consecutive spaces stays a
/// non-match against a simplified haystack.
loglib::CallbackStringRowPredicate::MatchFn MakeStringMatcher(
    const QString &pattern, loglib::LogConfiguration::LogFilter::Match match
)
{
    using Match = loglib::LogConfiguration::LogFilter::Match;
    switch (match)
    {
    case Match::Exactly:
    {
        // Capture by value: the parameter reference would dangle
        // after return. `QString`'s implicit sharing keeps this a
        // refcount bump.
        // clang-tidy flags `QString::operator==` and the QString
        // allocation as exception-escape; both are benign here.
        // NOLINTNEXTLINE(bugprone-exception-escape)
        return
            [pattern](std::string_view bytes) { return LogModel::ConvertToSingleLineCompactQString(bytes) == pattern; };
    }
    case Match::Contains:
    {
        // NOLINTNEXTLINE(bugprone-exception-escape)
        return [pattern](std::string_view bytes) {
            return LogModel::ConvertToSingleLineCompactQString(bytes).contains(pattern);
        };
    }
    case Match::RegularExpression:
    {
        QRegularExpression regex(pattern);
        PrimeRegex(regex);
        return [regex](std::string_view bytes) {
            return regex.match(LogModel::ConvertToSingleLineCompactQString(bytes)).hasMatch();
        };
    }
    case Match::Wildcard:
    {
        QRegularExpression regex(QRegularExpression::wildcardToRegularExpression(pattern));
        PrimeRegex(regex);
        return [regex](std::string_view bytes) {
            return regex.match(LogModel::ConvertToSingleLineCompactQString(bytes)).hasMatch();
        };
    }
    }
    return [](std::string_view) { return false; };
}

} // namespace

void MainWindow::UpdateFilters()
{
    // Sort filters cheapest-first so `std::ranges::all_of` short-
    // circuits on the cheapest rejecting test:
    //   1. EnumRowPredicate          - GetEnumValueId + bitset test
    //   2. TimeRangeRowPredicate     - GetValue + int compare
    //   3. CallbackStringRowPredicate - regex / UTF-8 walk
    // Tie-break on column index for deterministic, test-friendly order.
    using LogFilterType = loglib::LogConfiguration::LogFilter::Type;
    auto costOf = [](LogFilterType t) -> int {
        switch (t)
        {
        case LogFilterType::Enumeration:
            return 0;
        case LogFilterType::Time:
            return 1;
        case LogFilterType::String:
        default:
            return 2;
        }
    };
    std::vector<const loglib::LogConfiguration::LogFilter *> ordered;
    ordered.reserve(mFilters.size());
    for (const auto &filter : mFilters)
    {
        ordered.push_back(&filter.second);
    }
    std::ranges::sort(ordered, [&costOf](const auto *a, const auto *b) {
        const int costA = costOf(a->type);
        const int costB = costOf(b->type);
        if (costA != costB)
        {
            return costA < costB;
        }
        return a->row < b->row;
    });

    std::vector<loglib::RowPredicate> rules;
    rules.reserve(ordered.size());
    for (const loglib::LogConfiguration::LogFilter *filterPtr : ordered)
    {
        const loglib::LogConfiguration::LogFilter &filter = *filterPtr;
        const auto column = static_cast<size_t>(filter.row);
        switch (filter.type)
        {
        case LogFilterType::Time:
            rules.emplace_back(
                std::in_place_type<loglib::TimeRangeRowPredicate>, column, *filter.filterBegin, *filter.filterEnd
            );
            break;
        case LogFilterType::Enumeration:
        {
            // `filter` aliases `mFilters`, so the underlying strings
            // outlive the views. `EnumRowPredicate`'s constructor
            // copies/indexes them and keeps no reference back into the
            // span (pinned by the lifetime test in `test_log_filter.cpp`).
            std::vector<std::string_view> selectedViews;
            selectedViews.reserve(filter.filterValues.size());
            for (const std::string &v : filter.filterValues)
            {
                selectedViews.emplace_back(v);
            }
            const loglib::EnumDictionary *dictionary = ResolveEnumDictionary(filter.row);
            rules.emplace_back(
                std::in_place_type<loglib::EnumRowPredicate>,
                column,
                std::span<const std::string_view>(selectedViews),
                dictionary
            );
            break;
        }
        case LogFilterType::String:
        default:
            rules.emplace_back(
                std::in_place_type<loglib::CallbackStringRowPredicate>,
                column,
                MakeStringMatcher(QString::fromStdString(*filter.filterString), *filter.matchType)
            );
            break;
        }
    }
    mSortFilterProxyModel->SetFilterRules(std::move(rules));
}
