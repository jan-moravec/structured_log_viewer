#include "main_window.hpp"
#include "./ui_main_window.h"

#include "appearance_control.hpp"
#include "column_editor.hpp"
#include "columns_manager_dialog.hpp"
#include "configuration_diagnostics_dialog.hpp"
#include "filter_editor.hpp"
#include "network_stream_dialog.hpp"
#include "qt_streaming_log_sink.hpp"
#include "session_history_manager.hpp"
#include "streaming_control.hpp"

#include <loglib/bytes_producer.hpp>
#include <loglib/enum_dictionary.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/internal/ascii_case.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_processing.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/stream_line_source.hpp>
#include <loglib/tailing_bytes_producer.hpp>
#include <loglib/tcp_server_producer.hpp>
#include <loglib/udp_server_producer.hpp>

#include <QAbstractProxyModel>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QScopeGuard>
#include <QSignalBlocker>
#include <QStandardItemModel>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
#include <QTableView>
#include <QTimer>
#include <QUuid>
#include <QVBoxLayout>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <system_error>
#include <unordered_set>
#include <utility>
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

/// Decode the on-disk `Type::Boolean` filter (a subset of
/// `{"true", "false"}`) into the two-toggle pair
/// `BoolRowPredicate` consumes. Case-insensitive so a hand-edited
/// `"True"` / `"FALSE"` still loads.
struct BooleanFilterSides
{
    bool includeTrue = false;
    bool includeFalse = false;
};

BooleanFilterSides DecodeBooleanFilterSides(const std::vector<std::string> &filterValues) noexcept
{
    BooleanFilterSides sides;
    for (const std::string &v : filterValues)
    {
        if (loglib::internal::EqualsIgnoreCaseAscii(v, "true"))
        {
            sides.includeTrue = true;
        }
        else if (loglib::internal::EqualsIgnoreCaseAscii(v, "false"))
        {
            sides.includeFalse = true;
        }
    }
    return sides;
}

/// Why a saved `LogFilter` could not be revived. The load-side
/// validator uses this to summarise drops in one dialog.
enum class FilterValidationReason
{
    OutOfRangeRow,
    EmptyEnumSelection,
    TypeMismatch,
    MissingTimeRange,
    MissingNumericRange,
    MissingStringMatch,
    MissingBooleanSelection,
};

struct FilterValidationFailure
{
    FilterValidationReason reason;
    int row;
    /// The filter's column header, or empty when `row` is out of range.
    std::string columnHeader;
};

QString FilterValidationReasonString(FilterValidationReason reason)
{
    switch (reason)
    {
    case FilterValidationReason::OutOfRangeRow:
        return QStringLiteral("column index out of range");
    case FilterValidationReason::EmptyEnumSelection:
        return QStringLiteral("enumeration selection was empty (would hide every row)");
    case FilterValidationReason::TypeMismatch:
        return QStringLiteral("filter type does not match column type");
    case FilterValidationReason::MissingTimeRange:
        return QStringLiteral("time range is missing");
    case FilterValidationReason::MissingNumericRange:
        return QStringLiteral("numeric range is missing");
    case FilterValidationReason::MissingStringMatch:
        return QStringLiteral("string match is missing");
    case FilterValidationReason::MissingBooleanSelection:
        return QStringLiteral("no boolean side selected");
    }
    return QStringLiteral("unknown");
}

/// Check whether a saved filter still fits the current column
/// layout. Called from configuration load (drops failures into a
/// summary dialog) and from `MainWindow::AddFilter`'s pre-guard.
/// Returns `nullopt` on success.
std::optional<FilterValidationFailure> ValidateFilterAgainstColumns(
    const loglib::LogConfiguration::LogFilter &filter, const std::vector<loglib::LogConfiguration::Column> &columns
)
{
    using LogFilter = loglib::LogConfiguration::LogFilter;
    using ColumnType = loglib::LogConfiguration::Type;

    if (filter.row < 0 || static_cast<size_t>(filter.row) >= columns.size())
    {
        return FilterValidationFailure{
            .reason = FilterValidationReason::OutOfRangeRow, .row = filter.row, .columnHeader = std::string{}
        };
    }

    const auto &column = columns[static_cast<size_t>(filter.row)];

    if (filter.type == LogFilter::Type::Enumeration && filter.filterValues.empty())
    {
        return FilterValidationFailure{
            .reason = FilterValidationReason::EmptyEnumSelection, .row = filter.row, .columnHeader = column.header
        };
    }

    const bool isNumericColumn =
        column.type == ColumnType::Integer || column.type == ColumnType::Floating || column.type == ColumnType::Number;
    const bool isEnumLikeColumn = column.type == ColumnType::Enumeration || column.type == ColumnType::Level;
    const bool typesMatch = (filter.type == LogFilter::Type::Time && column.type == ColumnType::Time) ||
                            (filter.type == LogFilter::Type::Enumeration && isEnumLikeColumn) ||
                            (filter.type == LogFilter::Type::Boolean && column.type == ColumnType::Boolean) ||
                            (filter.type == LogFilter::Type::Number && isNumericColumn) ||
                            (filter.type == LogFilter::Type::String && column.type != ColumnType::Time &&
                             !isEnumLikeColumn && column.type != ColumnType::Boolean && !isNumericColumn);
    if (!typesMatch)
    {
        return FilterValidationFailure{
            .reason = FilterValidationReason::TypeMismatch, .row = filter.row, .columnHeader = column.header
        };
    }

    switch (filter.type)
    {
    case LogFilter::Type::Time:
        if (!filter.filterBegin.has_value() || !filter.filterEnd.has_value())
        {
            return FilterValidationFailure{
                .reason = FilterValidationReason::MissingTimeRange, .row = filter.row, .columnHeader = column.header
            };
        }
        break;
    case LogFilter::Type::Number:
        if (!filter.filterMinValue.has_value() && !filter.filterMaxValue.has_value())
        {
            return FilterValidationFailure{
                .reason = FilterValidationReason::MissingNumericRange, .row = filter.row, .columnHeader = column.header
            };
        }
        break;
    case LogFilter::Type::Boolean:
    {
        const BooleanFilterSides sides = DecodeBooleanFilterSides(filter.filterValues);
        if (!sides.includeTrue && !sides.includeFalse)
        {
            return FilterValidationFailure{
                .reason = FilterValidationReason::MissingBooleanSelection,
                .row = filter.row,
                .columnHeader = column.header
            };
        }
        break;
    }
    case LogFilter::Type::String:
        if (!filter.filterString.has_value() || !filter.matchType.has_value())
        {
            return FilterValidationFailure{
                .reason = FilterValidationReason::MissingStringMatch, .row = filter.row, .columnHeader = column.header
            };
        }
        break;
    case LogFilter::Type::Enumeration:
        // Empty-selection already handled above.
        break;
    }

    return std::nullopt;
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
    : MainWindow(nullptr, parent)
{
}

MainWindow::MainWindow(SessionHistoryManager *historyManager, QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), mHistoryManager(historyManager)
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
    // `modelReset` clears the header's hidden flags, but
    // `Column::visible` survives. Re-apply on every reset so load /
    // re-stream / teardown all stay consistent with the saved config.
    connect(mModel, &QAbstractItemModel::modelReset, this, &MainWindow::ApplyColumnVisibility);
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
    // `setSortRole` is intentionally not called: `LogFilterModel`
    // sorts via `loglib::CompareRows` straight against `LogTable`
    // (no `data(role)` round-trip), so the sort role no longer
    // drives behaviour. The deprecated no-op stays on the class for
    // one release to ease test / benchmark migration.
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
    mTableView->horizontalHeader()->setSectionsMovable(true);
    mTableView->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mTableView->horizontalHeader(), &QHeaderView::sectionMoved, this, &MainWindow::OnHeaderSectionMoved);
    connect(
        mTableView->horizontalHeader(),
        &QHeaderView::customContextMenuRequested,
        this,
        &MainWindow::ShowHeaderContextMenu
    );
    // Catch every column move (header drag and implicit moves like
    // mid-stream timestamp bubbling) so the runtime filter map and
    // proxy rules stay aligned with the source layout.
    connect(mModel, &QAbstractItemModel::columnsMoved, this, &MainWindow::OnSourceColumnsMoved);

    // Rebuild on demand. This is the only escape hatch when every
    // header section is hidden (right-click needs a visible section).
    connect(ui->menuView, &QMenu::aboutToShow, this, &MainWindow::RebuildViewMenu);

    mTableView->setShowGrid(true);
    mTableView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    mTableView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    connect(ui->actionNewSession, &QAction::triggered, this, &MainWindow::NewSession);
    connect(ui->actionNewWindow, &QAction::triggered, this, &MainWindow::NewWindow);
    // Greyed-out unless we have a `SessionHistoryManager` to thread
    // into the new window. `NewWindow()`'s nullptr-guard remains the
    // authoritative gate; this just turns the menu entry into a
    // visible affordance instead of a no-op click. Tests that
    // construct a `MainWindow` without a manager (the default
    // `init()` path) therefore see the action disabled.
    ui->actionNewWindow->setEnabled(mHistoryManager != nullptr);
    connect(ui->actionOpen, &QAction::triggered, this, &MainWindow::OpenFiles);
    connect(ui->actionOpenWithConfiguration, &QAction::triggered, this, &MainWindow::OpenWithConfiguration);

    // Lazily refresh the Recent Sessions submenu so other-window
    // mutations (auto-save in a sibling MainWindow, clear, evict) are
    // visible the next time the user opens the submenu without us
    // having to react to every `changed()` signal.
    if (ui->menuRecentSessions != nullptr)
    {
        // QMenu suppresses per-action tooltips unless this flag is
        // set; without it `RebuildRecentSessionsMenu`'s
        // primary-locator tooltips never reach the user and two
        // recents entries with the same `BuildLabel` output are
        // indistinguishable in the menu.
        ui->menuRecentSessions->setToolTipsVisible(true);
        connect(ui->menuRecentSessions, &QMenu::aboutToShow, this, &MainWindow::RebuildRecentSessionsMenu);
    }
    connect(ui->actionOpenLogStream, &QAction::triggered, this, &MainWindow::OpenLogStream);
    connect(ui->actionOpenNetworkStream, &QAction::triggered, this, &MainWindow::OpenNetworkStream);
    connect(ui->actionSaveConfiguration, &QAction::triggered, this, &MainWindow::SaveConfiguration);
    connect(ui->actionSaveSession, &QAction::triggered, this, &MainWindow::SaveSession);
    connect(ui->actionLoadConfiguration, &QAction::triggered, this, &MainWindow::LoadConfiguration);
    // "Exit" quits the whole application rather than just this
    // window (matches the typical File -> Exit affordance once the
    // app gained multi-window support).
    //
    // We pair `closeAllWindows` (which walks the top-level list and
    // fires `closeEvent` on each, giving every window's auto-save
    // flush a chance to run) with an explicit `quit` so the binary
    // also exits if a future change ever calls
    // `setQuitOnLastWindowClosed(false)` -- in that mode the
    // `closeAllWindows` walk on its own would leave a hidden,
    // running process behind. The pairing also keeps the `aboutToQuit`
    // safety-net in `main.cpp` engaged for windows whose close event
    // somehow declined to fire (e.g. a future overridden
    // `closeEvent` that calls `event->ignore()`).
    connect(ui->actionExit, &QAction::triggered, qApp, [] {
        QApplication::closeAllWindows();
        QApplication::quit();
    });

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

    // Record-detail dock: hidden by default; the View menu's Ctrl+I
    // toggle and row double-click both surface it.
    mRecordDetailDock = new RecordDetailDock(mModel, this);
    addDockWidget(Qt::RightDockWidgetArea, mRecordDetailDock);
    mRecordDetailDock->hide();
    // `actionToggleRecordDetails` is declared in `main_window.ui` but
    // not placed in any `<addaction>`, so uic doesn't add it to any
    // widget's `actions()`. A QAction's shortcut only fires once it
    // is associated with a widget; add it here so `Ctrl+I` is live
    // from a cold launch, before the View menu is ever opened.
    addAction(ui->actionToggleRecordDetails);
    connect(ui->actionToggleRecordDetails, &QAction::toggled, this, [this](bool on) {
        // Gate hidden->visible on a realised host window; see the
        // comment on `ShowRecordDetailsForProxyIndex`. The hide path
        // is always safe.
        if (on && !isVisible())
        {
            return;
        }
        mRecordDetailDock->setVisible(on);
        if (on)
        {
            mRecordDetailDock->raise();
        }
    });
    // Mirror dock visibility back onto the menu entry so the title-bar
    // X also un-checks it. `QSignalBlocker` breaks the toggled loop.
    connect(mRecordDetailDock, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        const QSignalBlocker blocker(ui->actionToggleRecordDetails);
        ui->actionToggleRecordDetails->setChecked(visible);
        if (visible)
        {
            UpdateRecordDetailsFromSelection();
        }
    });
    connect(mRecordDetailDock, &RecordDetailDock::openInNewWindowRequested, this, &MainWindow::OpenRecordDetailWindow);

    connect(mTableView, &QAbstractItemView::doubleClicked, this, &MainWindow::ShowRecordDetailsForProxyIndex);

    // Track selection changes through the live selection model;
    // centralised so a future `setModel` call only has to re-invoke
    // this helper.
    RebindRecordDetailSelectionTracking();

    // The dock owns its own `modelReset -> Clear` wiring, so reuse
    // outside `MainWindow` stays correct.

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

    mDiagnosticsButton = new QPushButton(this);
    mDiagnosticsButton->setObjectName(QStringLiteral("diagnosticsButton"));
    mDiagnosticsButton->setIcon(QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning));
    mDiagnosticsButton->setFlat(true);
    mDiagnosticsButton->setCursor(Qt::PointingHandCursor);
    mDiagnosticsButton->hide();
    statusBar()->addPermanentWidget(mDiagnosticsButton);
    connect(mDiagnosticsButton, &QPushButton::clicked, this, &MainWindow::ShowConfigurationDiagnostics);
    connect(mModel, &LogModel::columnHealthChanged, this, &MainWindow::UpdateDiagnosticsStatus);

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

        // Snapshot the just-finished mode before resetting it so the
        // auto-save gate below can distinguish "static load completed"
        // (worth recording in Recent Sessions) from "live-tail /
        // network stream completed" (transient, never re-openable
        // from a JSON snapshot). The same value is mirrored into
        // `mLastTerminalSessionMode` so `closeEvent` -> `AutoSave`
        // (which runs strictly after the live `mSessionMode` has
        // already been reset to `Idle`) still sees the real mode.
        const SessionMode justFinishedMode = mSessionMode;
        mLastTerminalSessionMode = mSessionMode;
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
        // Refresh the column-health snapshot now that parsing has
        // settled. Drives the header warning glyph and the status-bar
        // mismatch summary via `columnHealthChanged`.
        mModel->RefreshColumnHealth();
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
        // Keep `mCurrentSource` on Success / Cancelled (rows are
        // still present, descriptor still describes them); drop it
        // on Failed where there is nothing left to describe.
        if (result == StreamingResult::Failed)
        {
            mCurrentSource.reset();
        }

        // Auto-save the session snapshot on successful completion so
        // the user can reopen this exact view through Recent Sessions
        // and so the restore-on-launch flow has something to load.
        // `ShouldAutoSaveSession` filters out the cases where the
        // snapshot would never be re-openable -- no manager, no
        // source, network streams, live-tail file sessions.
        if (result == StreamingResult::Success && ShouldAutoSaveSession(justFinishedMode))
        {
            AutoSaveSessionSnapshot();
        }
    });
    connect(mModel, &LogModel::rotationDetected, this, &MainWindow::OnRotationDetected);
    connect(mModel, &LogModel::sourceStatusChanged, this, &MainWindow::OnSourceStatusChanged);
    // Keep enum filter bitsets and sort ranks in sync with the live
    // dictionary; scope the work to the reason that fired:
    //   - `Demoted`: the cached `EnumDictionary*` is now dangling.
    //     Flush the rank cache and rebuild predicates onto the
    //     string-set fallback.
    //   - `Promoted`: a column just gained a dictionary. Any
    //     `EnumRowPredicate` built earlier for that column was
    //     constructed with `dictionary = nullptr` and otherwise sits
    //     on the slow string-set fallback forever; rebuild so it
    //     picks up the bitset hot path. The signal doesn't say which
    //     column promoted, so any active enum filter triggers a
    //     rebuild. `EnumRankFor` self-heals on the next sort, so no
    //     cache flush is needed.
    //   - `Grew`: existing predicates still work (bitset for resolved
    //     ids, string-set fallback for unresolved). Rebuild only when
    //     a filter has unresolved values that may have just been
    //     interned -- the only case where rebuilding upgrades anything.
    connect(mModel, &LogModel::enumColumnsChanged, this, [this](EnumColumnsChangeReason reason, int columnIndex) {
        if (reason == EnumColumnsChangeReason::Demoted)
        {
            // Broad flush: rank cache keys alias across columns via
            // `EnumRankFor`, so invalidate everything to be safe.
            mSortFilterProxyModel->InvalidateEnumRanks();

            // A `Type::Level -> Type::String` demote orphans any saved
            // canonical-name filter (`"Info"`, ...) because those
            // strings never appear in the column's raw data. Translate
            // them to the raw entries `LogModel::AppendBatch` captured
            // pre-demote so the filter keeps matching the same rows.
            // Plain enum demotes need no translation;
            // `LastBatchLevelDemoteMappingFor` returns nullptr there.
            if (columnIndex >= 0)
            {
                if (const auto *levelMapping = mModel->LastBatchLevelDemoteMappingFor(columnIndex);
                    levelMapping != nullptr)
                {
                    const auto &columnsCfg = mModel->Configuration().columns;
                    const loglib::LogConfiguration::Column *demotedColumn =
                        std::cmp_less(columnIndex, columnsCfg.size()) ? &columnsCfg[static_cast<size_t>(columnIndex)]
                                                                      : nullptr;
                    for (auto &kv : mFilters)
                    {
                        loglib::LogConfiguration::LogFilter &filter = kv.second;
                        if (filter.row != columnIndex)
                        {
                            continue;
                        }
                        if (filter.type != loglib::LogConfiguration::LogFilter::Type::Enumeration)
                        {
                            continue;
                        }
                        // `ResolveLevel` still sees the column's saved
                        // `levelMapping` (the demote only flips
                        // `Column::type`), so user aliases like
                        // `"NOTICE" -> Info` survive.
                        std::vector<std::string> expanded;
                        for (const std::string &name : filter.filterValues)
                        {
                            std::optional<loglib::LogLevel> level;
                            if (demotedColumn != nullptr)
                            {
                                level = loglib::ResolveLevel(name, demotedColumn->levelMapping);
                            }
                            else
                            {
                                level = loglib::ParseLevelName(name);
                            }
                            if (!level.has_value())
                            {
                                continue;
                            }
                            const auto it = levelMapping->find(*level);
                            if (it == levelMapping->end())
                            {
                                continue;
                            }
                            for (const std::string &raw : it->second)
                            {
                                expanded.push_back(raw);
                            }
                        }
                        // Drop duplicates with a stable order.
                        std::ranges::sort(expanded);
                        const auto dupTail = std::ranges::unique(expanded);
                        expanded.erase(dupTail.begin(), dupTail.end());
                        // An empty `expanded` is a legitimate
                        // "matches nothing" outcome -- keep it so the
                        // rebuilt predicate rejects every row, matching
                        // the plain enum branch's empty-selection
                        // semantics.
                        filter.filterValues = std::move(expanded);
                    }
                    // Mirror once after the loop; the wire vector is
                    // snapshotted whole, so per-filter mirroring would
                    // redo the same work.
                    MirrorSessionStateToConfiguration();
                }
            }
        }
        // `columnIndex == -1` means "scope unknown" -- treat as
        // matches-anything to keep the safe broad behaviour.
        const auto matchesAffectedColumn = [columnIndex](const auto &kv) {
            return columnIndex < 0 || kv.second.row == columnIndex;
        };
        bool rebuild = false;
        switch (reason)
        {
        case EnumColumnsChangeReason::Demoted:
            rebuild = std::ranges::any_of(mFilters, [&matchesAffectedColumn](const auto &kv) {
                return kv.second.type == loglib::LogConfiguration::LogFilter::Type::Enumeration &&
                       matchesAffectedColumn(kv);
            });
            break;
        case EnumColumnsChangeReason::Promoted:
            rebuild = std::ranges::any_of(mFilters, [&matchesAffectedColumn](const auto &kv) {
                return kv.second.type == loglib::LogConfiguration::LogFilter::Type::Enumeration &&
                       matchesAffectedColumn(kv);
            });
            break;
        case EnumColumnsChangeReason::Grew:
            rebuild = std::ranges::any_of(mFilters, [this, &matchesAffectedColumn](const auto &kv) {
                return kv.second.type == loglib::LogConfiguration::LogFilter::Type::Enumeration &&
                       matchesAffectedColumn(kv) && !EnumFilterFullyResolved(kv.second);
            });
            break;
        }
        if (rebuild)
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
    // Sever the snapshot windows' `destroyed -> remove` hooks before
    // our members go away. Without this, the inherited `~QWidget`
    // child-destruction would fire each `destroyed` against an
    // already-destructed `mRecordDetailWindows`. Scoped disconnect
    // (by `QMetaObject::Connection`) so unrelated future `destroyed`
    // hooks can't be caught in the teardown.
    for (const auto &entry : std::as_const(mRecordDetailWindows))
    {
        disconnect(entry.destroyedConnection);
    }
    mRecordDetailWindows.clear();
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

    // Mirror `OpenFiles`: Shift forces replace; default appends onto
    // the active static session. `modifiers()` is the Qt 6 successor
    // to the deprecated `keyboardModifiers()` on `QDropEvent`.
    const bool forceReplace = event->modifiers().testFlag(Qt::ShiftModifier);
    StartStreamingOpenQueue(std::move(files), forceReplace ? OpenMode::Replace : OpenMode::Append);

    event->acceptProposedAction();
}

void MainWindow::UpdateUi()
{
    const QHeaderView *header = mTableView->horizontalHeader();
    const int columnCount = mTableView->model()->columnCount();
    // Skip the trailing column (it stretches to fill) and hidden
    // columns -- `resizeColumnToContents` walks the model even for
    // zero-width sections.
    for (int i = 0; i < columnCount - 1; ++i)
    {
        if (header != nullptr && header->isSectionHidden(i))
        {
            continue;
        }
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

void MainWindow::NewWindow()
{
    if (mHistoryManager == nullptr)
    {
        // No-history mode (test fixture / ad-hoc instance): refuse
        // to spawn extra windows. A test that needs multi-window can
        // construct them directly with the manager-aware constructor.
        return;
    }

    // Heap-allocated, no parent so the window is a top-level peer of
    // `this`. `WA_DeleteOnClose` lets Qt own the lifetime; closing
    // the new window unwinds it without needing an owner-side list.
    auto *child = new MainWindow(mHistoryManager, nullptr);
    child->setAttribute(Qt::WA_DeleteOnClose);
    child->show();
    child->raise();
    child->activateWindow();
}

void MainWindow::RebuildRecentSessionsMenu()
{
    if (ui->menuRecentSessions == nullptr)
    {
        return;
    }

    ui->menuRecentSessions->clear();

    if (mHistoryManager == nullptr)
    {
        QAction *placeholder = ui->menuRecentSessions->addAction(QStringLiteral("(history unavailable)"));
        placeholder->setEnabled(false);
        return;
    }

    const QList<RecentSessionEntry> entries = mHistoryManager->List();
    if (entries.isEmpty())
    {
        QAction *placeholder = ui->menuRecentSessions->addAction(QStringLiteral("(no recent sessions)"));
        placeholder->setEnabled(false);
        return;
    }

    for (const RecentSessionEntry &entry : entries)
    {
        QString label = entry.label;
        if (label.isEmpty())
        {
            label = entry.uuid;
        }
        QAction *action = ui->menuRecentSessions->addAction(label);
        // Tooltip carries the primary locator + total file count so
        // the user can disambiguate two entries that share a label.
        QString tooltip = entry.primaryLocator;
        if (entry.fileCount > 1)
        {
            tooltip += QStringLiteral(" (+ %1 more)").arg(entry.fileCount - 1);
        }
        if (!tooltip.isEmpty())
        {
            action->setToolTip(tooltip);
        }
        const QString uuid = entry.uuid;
        connect(action, &QAction::triggered, this, [this, uuid]() { OpenRecentSession(uuid); });
    }

    ui->menuRecentSessions->addSeparator();
    QAction *clearAction = ui->menuRecentSessions->addAction(QStringLiteral("Clear Recent Sessions"));
    // `menuRecentSessions->clear()` at the top of this rebuild deletes
    // every QAction we created on the previous show, which severs
    // these `connect`s -- no manual disconnect or leak management
    // needed across repeated submenu opens.
    connect(clearAction, &QAction::triggered, this, [this]() {
        if (mHistoryManager != nullptr)
        {
            mHistoryManager->Clear();
            // Drop our pinned uuid + open-windows membership in one
            // step. Sibling windows still hold their old uuids and
            // will re-populate the list on their next AutoSave; that
            // is the documented behaviour ("clear history" wipes the
            // store, not the live sessions).
            DetachAutoSaveUuid();
        }
    });
}

void MainWindow::OpenFilesForCli(const QStringList &files)
{
    if (files.isEmpty())
    {
        return;
    }
    // Mirror `OpenFiles`: a single positional argument is treated
    // as a possible configuration / session JSON before falling
    // back to opening it as a log file. This is the only heuristic
    // we have today: with multiple positional arguments we always
    // open them as log files. If a user wants to combine a
    // configuration with log files on the command line, they have
    // to load the configuration via `File -> Open with
    // Configuration...` once the window is up -- a future
    // dedicated flag (e.g. `--config <path>`) would route through
    // `DoLoadConfiguration` plus `StartStreamingOpenQueue(Append)`
    // exactly like that menu entry.
    if (files.size() == 1 && TryLoadAsConfiguration(files.front()))
    {
        UpdateUi();
        // Always hint after a configuration accept on the CLI:
        // `TryLoadAsConfiguration` never kicks off streaming on its
        // own (it only updates columns / filters / sort + sets
        // `mCurrentSource` for future Save Session), so the user is
        // staring at an empty view with column headers and no
        // visible indication that the binary actually accepted
        // their argument. Even a source-bound configuration ends
        // here without any rows on screen -- the source is mirrored
        // into `mCurrentSource` so File -> Open / Save Session
        // round-trip it, but no parse runs until the user picks a
        // file. The hint reads identically in both cases.
        const QString message =
            tr("Loaded '%1' as a configuration. Open log files (File -> Open...) to populate the view.")
                .arg(files.front());
        statusBar()->showMessage(message, STATUS_BAR_MESSAGE_TIMEOUT_MS);
        qInfo().noquote() << "OpenFilesForCli:" << message;
        return;
    }
    // Multi-file path: surface a diagnostic when the *first* file
    // looks like it would have been auto-detected as a configuration
    // had it been passed alone. Without this, a user running
    // `app cfg.json log.json` sees both files opened as logs (cfg
    // fails to parse and only `log.json` appears) with no hint that
    // the heuristic-skip is intentional.
    //
    // The probe is intentionally a substring peek (first 4 KB of
    // the file) rather than a full Glaze parse: the previous
    // implementation re-parsed the entire first file even when it
    // was a multi-megabyte JSONL log that just happened to end in
    // `.json` -- doubling the I/O on the largest argument the user
    // passed. False positives (a JSONL log whose first 4 KB contain
    // the literal string `"columns"`) only result in a stale
    // status-bar hint; the actual open path below still treats the
    // file as a log. The peek is paired with a leading-`{` shape
    // check so we don't fire on JSONL logs whose first line happens
    // to mention "columns" in a payload field.
    //
    // Surface to the status bar (and the console as a backup) so a
    // GUI-launched user actually sees the hint -- a plain
    // `qWarning()` only reaches whoever started the binary from a
    // terminal.
    if (files.size() > 1 && files.front().endsWith(QStringLiteral(".json"), Qt::CaseInsensitive))
    {
        QFile probe(files.front());
        if (probe.open(QIODevice::ReadOnly))
        {
            const QByteArray head = probe.read(4096);
            probe.close();

            // Skip leading whitespace so a pretty-printed file that
            // starts with `\n  {` still looks like a JSON object.
            int firstNonWs = 0;
            while (firstNonWs < head.size() && QChar::fromLatin1(head[firstNonWs]).isSpace())
            {
                ++firstNonWs;
            }
            const bool startsWithObject = firstNonWs < head.size() && head[firstNonWs] == '{';
            const bool mentionsColumns = head.contains("\"columns\"");
            if (startsWithObject && mentionsColumns)
            {
                const QString message =
                    tr("First argument '%1' looks like a configuration but is being opened as a log "
                       "file because additional arguments were passed. Use File -> Open with "
                       "Configuration... or pass the configuration on its own to apply it.")
                        .arg(files.front());
                statusBar()->showMessage(message, STATUS_BAR_MESSAGE_TIMEOUT_MS);
                qWarning().noquote() << "OpenFilesForCli:" << message;
            }
        }
        // QFile::open failure is silent: if we can't read the file,
        // the streaming open below will surface the error properly.
    }
    // Always Append: a user can drag-drop multiple files onto the
    // binary in one go without each clobbering the previous, and on
    // an empty-session start the Append branch lands at the same
    // place as a fresh open. There is no `Shift`-equivalent on the
    // CLI / forward path -- a `--new-instance` secondary launch
    // simply amends the primary's currently-active session. The user
    // opts into that trade-off by running with single-instance mode;
    // a future `--replace` flag could promote selected launches to
    // `OpenMode::Replace`, but the wire format does not yet carry it.
    StartStreamingOpenQueue(files, OpenMode::Append);
}

void MainWindow::RestoreLastSessionFromPath(const QString &jsonPath)
{
    if (jsonPath.isEmpty() || !QFileInfo::exists(jsonPath))
    {
        return;
    }
    if (!DoLoadConfiguration(jsonPath))
    {
        return;
    }

    // Pin the uuid (if the path matches one of our recents entries)
    // *before* we kick off streaming so an OS-quit / crash between
    // the load and the streaming-finished hook still restores this
    // window on next launch. We compute the uuid by stripping the
    // directory and the `.json` suffix -- per-uuid files always live
    // under `sessionsDir/<uuid>.json`. The stem must parse as a real
    // QUuid; otherwise the caller pointed us at an ad-hoc session
    // file outside the managed sessions dir, and pinning would let
    // the next AutoSave clobber that user file (or a future recents
    // entry whose name happens to collide with the stem).
    //
    // We do this *unconditionally* (even when the loaded config has
    // no source, see below) for parity with `OpenRecentSession`: a
    // configuration-only restore still belongs to the entry the
    // user clicked, so the next save should update that entry
    // rather than forking off a new one.
    if (mHistoryManager != nullptr)
    {
        const QFileInfo info(jsonPath);
        const QString stem = info.completeBaseName();
        const QUuid parsed = QUuid::fromString(stem);
        if (!parsed.isNull())
        {
            mAutoSaveUuid = stem;
            // Gate the `AddOpenWindowUuid` publish on `Touch`
            // confirming the manager actually owns this stem. The
            // QUuid::fromString check above only proves the filename
            // is uuid-shaped -- the JSON could still live outside
            // `sessionsDir` (caller passed an external path) or its
            // index entry could have been evicted by a sibling
            // process. In either case publishing the stem into
            // `openWindowsAtQuit` would leak a ghost uuid that the
            // next launch's fan-restore has to filter via
            // `QFileInfo::exists`. `Touch` returns true only when
            // the stem was found in the index, so the publish below
            // is restricted to entries we genuinely own.
            if (mHistoryManager->Touch(stem))
            {
                SessionHistoryManager::AddOpenWindowUuid(stem);
                mAutoSaveUuidPublished = true;
            }
        }
    }

    StreamFromCurrentSourceOrSkip(/*informIfNonFile=*/false);
}

void MainWindow::OpenRecentSession(const QString &uuid)
{
    if (mHistoryManager == nullptr || uuid.isEmpty())
    {
        return;
    }

    const QString jsonPath = mHistoryManager->PathForUuid(uuid);
    if (!QFileInfo::exists(jsonPath))
    {
        // The entry was evicted (or another process deleted the
        // backing JSON) between the menu rebuild and the click.
        // Surface a clear error and remove the dangling entry so the
        // next menu rebuild is clean.
        QMessageBox::warning(
            this,
            QStringLiteral("Recent Session Unavailable"),
            QStringLiteral("The JSON for this recent session has been removed. Dropping it from the list.")
        );
        mHistoryManager->Remove(uuid);
        return;
    }

    // Pre-flight parse before we touch the live model so a corrupt
    // recents file does not destroy the user's current view for
    // nothing. `NewSession` + `DoLoadConfiguration` below are both
    // destructive; calling them upfront would leave the window
    // blank-but-broken on parse failure.
    try
    {
        loglib::LogConfigurationManager probe;
        probe.Load(jsonPath.toStdString());
    }
    catch (const std::exception &e)
    {
        QMessageBox::warning(
            this,
            QStringLiteral("Cannot Open Recent Session"),
            QStringLiteral("Failed to parse '%1':\n%2").arg(jsonPath, QString::fromStdString(e.what()))
        );
        return;
    }

    // Step 1: discard the active session so the restored columns /
    // filters / sort do not collide with the live state. We pick the
    // destructive path on purpose: Recent Sessions is "open this exact
    // view", not "merge into the current one". `NewSession` also
    // detaches our previous `mAutoSaveUuid`; we re-pin to @p uuid
    // below once the load succeeds.
    NewSession();

    // Step 2: load the configuration (this also seeds the source
    // descriptor + filter list + sort).
    if (!DoLoadConfiguration(jsonPath))
    {
        // Defensive: pre-flight already accepted the file; failing
        // here implies a TOCTOU race. Bail without queueing files.
        return;
    }

    // Step 3: pin the recents uuid first so a crash / OS-quit
    // between here and the streaming-finished hook still restores
    // this window on next launch (matches `RestoreLastSessionFromPath`).
    // Gate the publish on `Touch` confirming the index still owns
    // @p uuid -- between the `QFileInfo::exists` check above and
    // here, a sibling process could have `Remove`d the entry. The
    // file we already loaded survives, but publishing the uuid
    // would leak a ghost entry that the next launch's fan-restore
    // has to filter away.
    mAutoSaveUuid = uuid;
    if (mHistoryManager->Touch(uuid))
    {
        SessionHistoryManager::AddOpenWindowUuid(uuid);
        mAutoSaveUuidPublished = true;
    }

    // Step 4: open the source locators captured in the configuration.
    // `mCurrentSource` was just populated by the load above. The
    // user clicked an explicit recents entry, so the non-File
    // branch is worth surfacing as a `QMessageBox` rather than
    // silently dropped.
    StreamFromCurrentSourceOrSkip(/*informIfNonFile=*/true);
}

void MainWindow::StreamFromCurrentSourceOrSkip(bool informIfNonFile)
{
    if (!mCurrentSource.has_value() || mCurrentSource->locators.empty())
    {
        // Configuration without a source: columns / filters are
        // installed but there is nothing to stream. Caller treats
        // this like a plain Load Configuration call.
        return;
    }

    if (mCurrentSource->kind != loglib::LogConfiguration::Source::Kind::File)
    {
        // NetworkStream snapshots persist the producer URI as the
        // locator. Feeding that to `StartStreamingOpenQueue` (which
        // opens its arguments as static files) would try to parse
        // `tcp://...` as a file path and surface a bogus parse
        // error. Newer builds avoid auto-saving stream sessions
        // (see `ShouldAutoSaveSession`), but older recents JSON
        // captured before this gate may still carry stream sources.
        // The user can manually re-bind via Open Network Stream...
        if (informIfNonFile)
        {
            QMessageBox::information(
                this,
                QStringLiteral("Network Stream Session"),
                QStringLiteral("This recent session was a network stream; the columns and filters have been "
                               "restored, but the producer must be re-bound manually via 'Open Network Stream...'.")
            );
        }
        return;
    }

    QStringList files;
    files.reserve(static_cast<qsizetype>(mCurrentSource->locators.size()));
    for (const std::string &locator : mCurrentSource->locators)
    {
        files.append(QString::fromStdString(locator));
    }

    // Append mode so the freshly-loaded filters survive into the new
    // streamed rows (see `OpenWithConfiguration` for the same
    // rationale). With `mSessionMode == Idle` and the model empty,
    // the Append branch is a no-op for state and the first file goes
    // through `BeginStreaming` cleanly.
    StartStreamingOpenQueue(files, OpenMode::Append);
}

void MainWindow::NewSession()
{
    // Tear down whatever was loaded: rows, runtime filters, source
    // descriptor, session mode. Configuration columns are intentionally
    // preserved -- the next open / load can reuse the layout the user
    // has been editing. To wipe columns too, the user loads a fresh
    // configuration file.
    //
    // `LogModel::Reset` already handles an orderly producer stop +
    // sink drain for live-tail / streaming sessions, so we do not
    // need a separate branch for `IsStreamingActive`.
    mModel->Reset();
    ClearAllFilters();
    mCurrentSource.reset();
    mSessionMode = SessionMode::Idle;
    // Forget what the last session was: the next closeEvent's
    // `AutoSaveSessionSnapshot` should treat us as Idle, not as
    // "still carries the previous (possibly live-tail) mode".
    mLastTerminalSessionMode = SessionMode::Idle;
    mStreamingFileName.clear();
    mStreamingLineCount = 0;
    mStreamingErrorCount = 0;
    mFirstStreamingBatchSeen = false;
    mSourceWaiting = false;
    // Drop the pinned recents uuid so the next AutoSave creates a
    // fresh entry instead of silently overwriting the previous
    // session's JSON (which would erase it from history). Also drop
    // ourselves from the open-windows-at-quit list so a crash before
    // the next AutoSave doesn't re-restore the just-discarded session.
    DetachAutoSaveUuid();
    SetConfigurationUiEnabled(true);
    UpdateStreamToolbarVisibility();
    UpdateStreamingStatus();
    UpdateUi();
}

void MainWindow::OpenFiles()
{
    // Sample the modifier state *before* the modal file dialog
    // appears: the dialog yields the keyboard back to the user and
    // by the time it returns, `keyboardModifiers()` reports whatever
    // is currently held -- which is approximately never the Shift
    // they were holding when they clicked the menu entry. Sampling
    // here matches the "Shift on menu activation forces Replace"
    // gesture the tooltip advertises.
    const bool forceReplace = QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);

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

    StartStreamingOpenQueue(files, forceReplace ? OpenMode::Replace : OpenMode::Append);
}

void MainWindow::OpenWithConfiguration()
{
    // Step 1: prompt for the configuration or session JSON.
    const QString configPath = QFileDialog::getOpenFileName(
        this, "Select Configuration or Session", QString(), "JSON (*.json);;All Files (*)"
    );
    if (configPath.isEmpty())
    {
        return;
    }

    // Pre-flight the parse into a throw-away manager so we never
    // mutate the live model with a config whose load we then have
    // to "undo" on cancel / failure. `DoLoadConfiguration` is
    // unconditionally destructive (it clears proxy rules + sort
    // before the load), so calling it before both dialogs return
    // would strand the user in a half-loaded state if they cancel
    // the file picker.
    try
    {
        loglib::LogConfigurationManager probe;
        probe.Load(configPath.toStdString());
    }
    catch (const std::exception &e)
    {
        QMessageBox::critical(
            this,
            QStringLiteral("Cannot Open Configuration"),
            QStringLiteral("Failed to parse '%1':\n%2").arg(configPath, QString::fromStdString(e.what()))
        );
        return;
    }

    // Step 2: prompt for log files. Cancelling here is a graceful
    // exit -- the current view is untouched.
    const QStringList files = QFileDialog::getOpenFileNames(
        this, "Select Log Files to Open with Configuration", QString(), "All Files (*.*)"
    );
    if (files.isEmpty())
    {
        return;
    }

    // Both prompts confirmed. If a streaming parse is currently in
    // flight, defer the destructive `DoLoadConfiguration` until it
    // finishes: the worker thread is reading the *current* column
    // layout from the configuration snapshot it captured at
    // `BeginStreaming` time, and rewriting that layout from under
    // it would surface as columns shifting / disappearing mid-parse
    // and (worse) cached `EnumDictionary` indices pointing at the
    // wrong column. We surface the deferral as a status-bar hint
    // so the user understands why the new config / files do not
    // appear immediately.
    if (mModel->IsStreamingActive())
    {
        const QString message =
            tr("Configuration will apply once the current parse completes.");
        statusBar()->showMessage(message, STATUS_BAR_MESSAGE_TIMEOUT_MS);
        // One-shot connect: fires on the next `streamingFinished`
        // and disconnects automatically. `Qt::SingleShotConnection`
        // is the canonical way to express "run once and tear down"
        // without juggling a `QMetaObject::Connection`.
        QObject::connect(
            mModel,
            &LogModel::streamingFinished,
            this,
            [this, configPath, files](StreamingResult /*result*/) {
                if (!DoLoadConfiguration(configPath))
                {
                    // TOCTOU race between the dialog accept and the
                    // deferred apply: the file was rewritten in the
                    // meantime. Surface to the user and abandon the
                    // queue rather than silently dropping the load.
                    QMessageBox::warning(
                        this,
                        QStringLiteral("Cannot Open with Configuration"),
                        QStringLiteral(
                            "Configuration '%1' could no longer be parsed when the active "
                            "parse completed. Re-select the file from File -> Open with "
                            "Configuration..."
                        ).arg(configPath)
                    );
                    return;
                }
                StartStreamingOpenQueue(files, OpenMode::Append);
            },
            Qt::SingleShotConnection
        );
        return;
    }

    // No streaming in flight: commit the config load, then queue
    // the files in Append mode so the freshly-restored filters /
    // sort survive into the new session.
    if (!DoLoadConfiguration(configPath))
    {
        // Defensive: the pre-flight already accepted this file, so
        // a second-stage failure here would be a TOCTOU race
        // (file rewritten between the two reads). Surface to the
        // user and bail without queueing anything.
        return;
    }
    StartStreamingOpenQueue(files, OpenMode::Append);
}

bool MainWindow::TryLoadAsConfiguration(const QString &file)
{
    // Probe-first: parse into a throw-away manager so the live
    // proxy / sort / model are never touched if the file isn't
    // actually a configuration. The previous in-place `Load` had
    // two failure modes the throw-away avoids:
    //   1. The destructive pre-clears (`SetFilterRules({})` +
    //      `sortByColumn(-1, ...)`) ran *before* the parse, so a
    //      throwing parse would leak: `mFilters` survived but the
    //      proxy's compiled rules were gone, silently disabling
    //      every active filter for the caller's Append fall-back.
    //   2. Glaze accepts `{}` (and any object containing only
    //      session-only fields) as a default-valued
    //      `LogConfiguration` -- columns empty. Treating that as a
    //      "configuration" and applying it would wipe the user's
    //      column layout to the default. We additionally reject
    //      column-less parses below as "not a configuration", so
    //      the caller falls through to opening the file as a log.
    try
    {
        loglib::LogConfigurationManager probe;
        probe.Load(file.toStdString());
        if (probe.Configuration().columns.empty())
        {
            // A configuration with zero columns is indistinguishable
            // from a default-constructed one and would clobber the
            // current view. Treat as a probe miss.
            return false;
        }
    }
    catch (...)
    {
        return false;
    }

    // Probe accepted: commit destructive changes. From here on a
    // throw is a TOCTOU race (the file changed between the two
    // reads); we surface it via the same `false` return as a probe
    // miss but the live state may already be partially mutated.
    try
    {
        // Drop proxy rules and the sort *before* the in-place
        // `Load` rewrites the configuration: existing rules /
        // `mSortColumn` were built for the old column layout and
        // would otherwise evaluate against the wrong columns under
        // the upcoming model reset. `mFilters` itself is rebuilt
        // below by `RebuildFiltersFromConfiguration`.
        mSortFilterProxyModel->SetFilterRules({});
        mTableView->sortByColumn(-1, Qt::AscendingOrder);

        mModel->ConfigurationManager().Load(file.toStdString());
        // Mirror the rationale in `DoLoadConfiguration`: a single-
        // file open / drop that matches a configuration is a
        // session boundary, so drop the previous session's recents
        // pin before any subsequent AutoSave can rewrite that
        // unrelated session's JSON in place under the stale uuid.
        // The probe-miss case above already returned without
        // touching `mAutoSaveUuid`, so a non-configuration drop
        // still preserves the prior pin for the caller's Append
        // fall-back.
        DetachAutoSaveUuid();
        // `Load` rewrites the configuration without emitting any
        // model signal; the reset re-initialises the header and
        // pulls the loaded `visible` flags via the wired
        // `modelReset -> ApplyColumnVisibility` connect.
        mModel->NotifyConfigurationReplaced();

        // Restore the persisted sort *before* RebuildFiltersFromConfiguration,
        // because that helper re-mirrors session state and would
        // otherwise overwrite the loaded sort with the cleared
        // proxy sort. Columns-only files default to `-1` (no sort).
        const auto loadedSort = mModel->Configuration().sort;
        if (loadedSort.columnIndex >= 0 && loadedSort.columnIndex < mModel->columnCount())
        {
            mTableView->sortByColumn(
                loadedSort.columnIndex, loadedSort.descending ? Qt::DescendingOrder : Qt::AscendingOrder
            );
        }

        // Mirror the loaded source descriptor so the next session
        // save round-trips it. We deliberately do not auto-bind.
        mCurrentSource = mModel->Configuration().source;

        RebuildFiltersFromConfiguration();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void MainWindow::StartStreamingOpenQueue(QStringList files, OpenMode mode)
{
    // Live-tail and network sessions are inherently single-source: any
    // new static-files open implicitly tears them down and starts
    // fresh, regardless of @p mode. Static sessions honour the caller.
    const bool destructive = (mode == OpenMode::Replace) || (mSessionMode == SessionMode::LiveTail);

    if (destructive)
    {
        // Mirror the historic open path: drop rows, runtime filters,
        // and the source descriptor before queueing. `mModel->Reset()`
        // stops any in-flight streaming worker synchronously, so the
        // destructive branch is safe even mid-stream.
        mModel->Reset();
        ClearAllFilters();
        mCurrentSource.reset();
        mSessionMode = SessionMode::Idle;
        // Forget the just-discarded session's mode so a closeEvent
        // before the next streamingFinished does not consult a stale
        // `mLastTerminalSessionMode` from the previous session.
        mLastTerminalSessionMode = SessionMode::Idle;
        // Drop the previous session's recents uuid so the next
        // AutoSave creates a fresh entry instead of overwriting (and
        // erasing) the prior session's JSON in place.
        DetachAutoSaveUuid();
    }
    else if (mModel->IsStreamingActive())
    {
        // Append onto an in-flight static session: queue and bail.
        // The `streamingFinished` lambda in this class's constructor
        // already drains `mPendingOpenFiles` via
        // `StreamNextPendingFile` once the active worker terminates.
        // Starting another worker here would trip
        // `LogModel::AppendStreaming`'s
        // `!mStreamingWatcher->isRunning()` assert in debug builds
        // and silently abandon the in-flight worker (rows lost, no
        // `streamingFinished` for the original parse) in release.
        // We `append` rather than overwrite so an existing tail-end
        // queue from the current `streamingFinished` drain survives.
        mPendingOpenFiles.append(std::move(files));
        return;
    }
    else if (mSessionMode == SessionMode::Idle && mModel->rowCount() > 0)
    {
        // Append into a previously-finished static session: the
        // streamingFinished handler flipped `mSessionMode` back to
        // `Idle`, but the rows / filters / source from the prior
        // session are still in place. Re-arm `Static` so that
        // `StreamNextPendingFile` routes the new files through
        // `AppendStreaming` instead of the row-clearing
        // `BeginStreaming` path.
        mSessionMode = SessionMode::Static;
    }
    // Otherwise (Idle + empty model): leave mode at Idle. The
    // upcoming `StreamNextPendingFile` will take the
    // `isFirstFileInSession` branch and `BeginStreaming` the queue,
    // which preserves runtime filters loaded via
    // "Open with Configuration..." because we never called
    // `ClearAllFilters` in the non-destructive branch.

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
        // Multi-file sessions record every appended file path in load
        // order so SaveSession + RecentSessions can reopen the full
        // set. The first file initialises the descriptor; subsequent
        // files push onto the existing locators vector.
        if (isFirstFileInSession)
        {
            mCurrentSource = loglib::LogConfiguration::Source{
                .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {file.toStdString()}
            };
        }
        else if (mCurrentSource.has_value() && mCurrentSource->kind == loglib::LogConfiguration::Source::Kind::File)
        {
            mCurrentSource->locators.push_back(file.toStdString());
        }
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
    OpenLogStreamFromPath(file);
}

void MainWindow::OpenLogStreamFromPath(const QString &file)
{
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

    // Flush the *outgoing* session so any user edits made after its
    // last `streamingFinished` (filter tweaks, sort changes) survive
    // the destructive reset below. `AutoSaveSessionSnapshot` is a
    // no-op when the previous session was itself live-tail or had no
    // pinned uuid -- the only path that actually writes is "Static
    // session with edits"; live-tail-to-live-tail switches stay
    // free. `publishOpenWindow=false` because we immediately
    // `DetachAutoSaveUuid()` afterwards.
    AutoSaveSessionSnapshot(/*publishOpenWindow=*/false);

    // Mirror the static-open reset so residual state cannot leak in.
    mModel->Reset();
    ClearAllFilters();
    // Detach the previous static session's recents uuid: a live-tail
    // stream is transient and we deliberately do not auto-save it,
    // but leaving `mAutoSaveUuid` pinned to the prior static session
    // would cause `closeEvent`'s flush + `RemoveOpenWindowUuid` to
    // operate on a stale uuid (and drop the prior session from the
    // multi-window restore set even though the user did not discard
    // it -- they only switched to a stream view).
    DetachAutoSaveUuid();

    mStreamingFileName = QFileInfo(file).fileName();
    mCurrentSource = loglib::LogConfiguration::Source{
        .kind = loglib::LogConfiguration::Source::Kind::File, .locators = {file.toStdString()}
    };
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

void MainWindow::OpenLogStreamForTest(const QString &filePath)
{
    OpenLogStreamFromPath(filePath);
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

    // Same rationale as `OpenLogStream`: flush any unsaved edits to
    // the outgoing static session before the destructive reset.
    AutoSaveSessionSnapshot(/*publishOpenWindow=*/false);

    // Mirror the OpenLogStream reset.
    mModel->Reset();
    ClearAllFilters();
    // Same rationale as `OpenLogStream`: drop the previous static
    // session's pinned uuid so the network-stream view does not
    // accidentally inherit (or drop) the recents entry it has no
    // relationship to.
    DetachAutoSaveUuid();

    mStreamingFileName = QString::fromStdString(displayName);
    mCurrentSource = loglib::LogConfiguration::Source{
        .kind = loglib::LogConfiguration::Source::Kind::NetworkStream, .locators = {displayName}
    };
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
    // Tear down but keep visible rows so the user can keep working
    // on them. `mCurrentSource` survives -- those rows still came
    // from that source.
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
    ui->actionSaveSession->setEnabled(enabled);
    ui->actionPreferences->setEnabled(enabled);
    // Reorder + right-click are gated mid-stream because
    // `LogModel::MoveColumn` rotates `columns` while the streaming
    // pipeline is busy mutating it through `AppendKeys`; a drag
    // between batches would scramble the proxy chain's column-keyed
    // state. The `View` menu stays reachable (only flips the
    // `visible` flag).
    if (QHeaderView *header = mTableView->horizontalHeader(); header != nullptr)
    {
        header->setSectionsMovable(enabled);
        header->setContextMenuPolicy(enabled ? Qt::CustomContextMenu : Qt::NoContextMenu);
    }
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
    if (name == QStringLiteral("actionNewSession"))
    {
        return ui->actionNewSession;
    }
    if (name == QStringLiteral("actionNewWindow"))
    {
        return ui->actionNewWindow;
    }
    if (name == QStringLiteral("actionOpen"))
    {
        return ui->actionOpen;
    }
    if (name == QStringLiteral("actionOpenWithConfiguration"))
    {
        return ui->actionOpenWithConfiguration;
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
    if (name == QStringLiteral("actionSaveSession"))
    {
        return ui->actionSaveSession;
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
    if (name == QStringLiteral("actionToggleRecordDetails"))
    {
        return ui->actionToggleRecordDetails;
    }
    return nullptr;
}

QMenu *MainWindow::FiltersMenu() const
{
    return ui->menuFilters;
}

QMenu *MainWindow::ViewMenu() const
{
    return ui->menuView;
}

QMenu *MainWindow::FilterSubMenu(const QString &filterID) const
{
    const auto it = mFilterSubMenus.find(filterID.toStdString());
    if (it == mFilterSubMenus.end())
    {
        return nullptr;
    }
    return it->second;
}

#ifdef LOGAPP_BUILD_TESTING
QList<RecordDetailWindow *> MainWindow::RecordDetailWindowsForTest() const
{
    // Materialise the live snapshot-window set out of the heap-keyed
    // tracker. The tracker is keyed on the original heap address (a
    // `quintptr`) but iteration order isn't stable across runs; tests
    // only assert on count and per-window state, never on identity.
    QList<RecordDetailWindow *> windows;
    windows.reserve(mRecordDetailWindows.size());
    for (const auto &entry : std::as_const(mRecordDetailWindows))
    {
        if (!entry.window.isNull())
        {
            windows.append(entry.window.data());
        }
    }
    return windows;
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

bool MainWindow::TryLoadAsConfigurationForTest(const QString &file)
{
    return TryLoadAsConfiguration(file);
}

void MainWindow::SetConfigurationUiEnabledForTest(bool enabled)
{
    SetConfigurationUiEnabled(enabled);
}

void MainWindow::SaveConfigurationToPathForTest(const QString &path, loglib::SaveScope scope)
{
    DoSaveConfiguration(path, scope);
}

void MainWindow::LoadConfigurationFromPathForTest(const QString &path)
{
    DoLoadConfiguration(path);
}

void MainWindow::SetSuppressDialogsForTest(bool suppress)
{
    mSuppressDialogsForTest = suppress;
}

int MainWindow::LastDroppedFilterCountForTest() const
{
    return mLastDroppedFilterCountForTest;
}

void MainWindow::SetCurrentSourceForTest(std::optional<loglib::LogConfiguration::Source> source)
{
    mCurrentSource = std::move(source);
}

void MainWindow::OpenFilesForTest(const QStringList &files, OpenMode mode)
{
    StartStreamingOpenQueue(files, mode);
}

bool MainWindow::OpenWithConfigurationForTest(const QString &configPath, const QStringList &files)
{
    if (!DoLoadConfiguration(configPath))
    {
        return false;
    }
    if (!files.isEmpty())
    {
        StartStreamingOpenQueue(files, OpenMode::Append);
    }
    return true;
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

void MainWindow::ShowDroppedFiltersDialog(int droppedCount, const QString &message)
{
#ifdef LOGAPP_BUILD_TESTING
    mLastDroppedFilterCountForTest = droppedCount;
    if (mSuppressDialogsForTest)
    {
        // Skip the modal so the offscreen-QPA test thread does not
        // block; the count is what the tests assert against.
        return;
    }
#else
    (void)droppedCount;
#endif
    QMessageBox::warning(this, QStringLiteral("Filters Dropped on Load"), message);
}

void MainWindow::MirrorSessionStateToConfiguration()
{
    // Snapshot the runtime filter map into the wire-format vector so
    // `Save` and the lib-side `MoveColumn` row-remap see the live
    // set. UUIDs are GUI-internal and regenerated on load.
    //
    // Sort by (row, type, payload) so two consecutive saves write
    // byte-identical JSON and the Filters-menu order on the
    // round-trip is stable.
    std::vector<loglib::LogConfiguration::LogFilter> snapshot;
    snapshot.reserve(mFilters.size());
    for (const auto &[id, f] : mFilters)
    {
        snapshot.push_back(f);
    }
    using LogFilter = loglib::LogConfiguration::LogFilter;
    std::ranges::sort(snapshot, [](const LogFilter &a, const LogFilter &b) {
        // Compare `(row, type)` head-on so we never have to alias an
        // `enum class` value as an int reference (which would be
        // implementation-defined). Tail fields are `std::optional` /
        // `std::vector` and already define a lexicographic `<`, so
        // `std::tie` gives the same byte-identical ordering the
        // longer manual chain produced.
        if (a.row != b.row)
        {
            return a.row < b.row;
        }
        if (a.type != b.type)
        {
            return static_cast<int>(a.type) < static_cast<int>(b.type);
        }
        return std::tie(
                   a.filterString, a.matchType, a.filterBegin, a.filterEnd, a.filterMinValue, a.filterMaxValue,
                   a.filterValues
               ) <
               std::tie(
                   b.filterString, b.matchType, b.filterBegin, b.filterEnd, b.filterMinValue, b.filterMaxValue,
                   b.filterValues
               );
    });
    mModel->ConfigurationManager().SetFilters(std::move(snapshot));

    // Sort: read live from the proxy so the persisted value matches
    // what the user sees in the header indicator.
    loglib::LogConfiguration::Sort sort;
    sort.columnIndex = mSortFilterProxyModel->SortColumn();
    sort.descending = (mSortFilterProxyModel->SortOrder() == Qt::DescendingOrder);
    mModel->ConfigurationManager().SetSort(sort);

    // Drop empty-locator Sources before mirroring: the on-disk
    // schema documents `source` as "omit when no source is bound",
    // so a `Source{kind: File, locators: {}}` is meaningless to a
    // re-loading window (kind without an address is not actionable)
    // and round-trips into the recents UI as a label-less entry.
    // `SetSource(nullopt)` keeps the loaded configuration shape
    // consistent with the snapshots `WriteSnapshot` would later
    // index against.
    if (mCurrentSource.has_value() && !mCurrentSource->locators.empty())
    {
        mModel->ConfigurationManager().SetSource(mCurrentSource);
    }
    else
    {
        mModel->ConfigurationManager().SetSource(std::nullopt);
    }
}

bool MainWindow::ShouldAutoSaveSession(SessionMode justFinishedMode) const
{
    if (mHistoryManager == nullptr)
    {
        return false;
    }
    if (!mCurrentSource.has_value() || mCurrentSource->locators.empty())
    {
        // Nothing to round-trip; a session entry without a source
        // can't be reopened by Recent Sessions and would just
        // confuse the menu.
        return false;
    }
    if (mCurrentSource->kind != loglib::LogConfiguration::Source::Kind::File)
    {
        // NetworkStream sessions are inherently transient: the
        // locator is a producer URI / display name, not something
        // `StartStreamingOpenQueue` can re-bind on a future launch.
        return false;
    }
    if (justFinishedMode == SessionMode::LiveTail)
    {
        // Live-tail-on-a-file sessions look like a static `File`
        // source on disk (same kind, same locator) but they bind a
        // `TailingBytesProducer`, not a one-shot parser. Re-opening
        // the snapshot would silently downgrade the user from
        // tailing to a static load -- worse than not auto-saving at
        // all.
        return false;
    }
    return true;
}

void MainWindow::AutoSaveSessionSnapshot(bool publishOpenWindow)
{
    // Prefer the live `mSessionMode` (e.g. closeEvent fires
    // mid-stream while we're still `Static` / `LiveTail`), falling
    // back to the mode captured just before the last
    // `streamingFinished` reset. Without the fallback, a closeEvent
    // that fires after a live-tail finished would see
    // `mSessionMode == Idle`, slip past the `justFinishedMode ==
    // LiveTail` guard in `ShouldAutoSaveSession`, and write a
    // phantom Recent Sessions entry pointing at a tailing producer
    // that cannot be re-bound on restore.
    const SessionMode effectiveMode =
        (mSessionMode != SessionMode::Idle) ? mSessionMode : mLastTerminalSessionMode;
    if (!ShouldAutoSaveSession(effectiveMode))
    {
        return;
    }

    // Snapshot the live filters / sort / source into the model's
    // configuration manager. Mirrors the manual `SaveSession` path so
    // auto-save and a user-driven save produce the same JSON.
    MirrorSessionStateToConfiguration();

    const loglib::LogConfiguration &configuration = mModel->ConfigurationManager().Configuration();
    const QString uuid = mHistoryManager->WriteSnapshot(configuration, mAutoSaveUuid);
    if (uuid.isEmpty())
    {
        // Save failed (serialization error, lock acquisition timeout,
        // I/O error, ...). Recovery is implicit: the atomic
        // temp+rename in `LogConfigurationManager::Save` guarantees
        // that any pre-existing `<uuid>.json` is *not* overwritten
        // on failure, so the previously-persisted state survives.
        // The `mAutoSaveUuid` / `openWindowsAtQuit` pins (if any)
        // still point at that valid JSON, so no extra cleanup is
        // required here.
        return;
    }
    // Pin the uuid so subsequent auto-saves rewrite the same JSON
    // instead of cluttering recents with one entry per save.
    mAutoSaveUuid = uuid;
    if (publishOpenWindow)
    {
        // Eagerly publish ourselves into the open-windows set so a
        // crash / OS quit between now and `closeEvent` restores this
        // window on next launch. The `closeEvent` flush passes
        // `publishOpenWindow=false` because it immediately removes
        // the uuid again.
        SessionHistoryManager::AddOpenWindowUuid(uuid);
        mAutoSaveUuidPublished = true;
    }
}

void MainWindow::DetachAutoSaveUuid()
{
    if (mAutoSaveUuid.isEmpty())
    {
        return;
    }
    // Skip the cross-process `RemoveOpenWindowUuid` round-trip when
    // we never published the uuid in the first place. Common case:
    // `closeEvent` runs `AutoSaveSessionSnapshot(false)` (flush
    // only) and then this method on a window that never reached a
    // `streamingFinished` (so the per-window publish from the
    // default-`true` snapshot path never fired). The Remove would
    // be a no-op on the QSettings key but still pays the
    // `QLockFile` acquisition + `sync()` cost.
    if (mAutoSaveUuidPublished)
    {
        SessionHistoryManager::RemoveOpenWindowUuid(mAutoSaveUuid);
        mAutoSaveUuidPublished = false;
    }
    mAutoSaveUuid.clear();
}

QString MainWindow::RestorableActiveSessionUuid() const noexcept
{
    // Mirror the `ShouldAutoSaveSession` gates: a uuid is only
    // worth persisting into `openWindowsAtQuit` if fan-restore can
    // actually do something useful with it on the next launch.
    // Live-tail (`SessionMode::LiveTail`) is intentionally accepted
    // here even though `ShouldAutoSaveSession` rejects it -- the
    // uuid was pinned by a previous static load (live-tail starts
    // from a static `File` source), and the JSON on disk reflects
    // that static state. Restoring it gives the user back the
    // static view of the same file, which is closer to what they
    // had than nothing. The auto-save gate's stricter rule is about
    // not *creating* fresh stream snapshots; the restore gate's
    // looser rule is about not *losing* state at OS-quit time.
    if (mAutoSaveUuid.isEmpty())
    {
        return {};
    }
    if (!mCurrentSource.has_value() || mCurrentSource->locators.empty())
    {
        // No-source configurations are intentionally restorable
        // (the user can pin a columns-only view), but only when a
        // uuid was explicitly pinned by `RestoreLastSessionFromPath`
        // / `OpenRecentSession`. If we get here `mAutoSaveUuid` is
        // set and there is no source -- that case (pinned uuid, no
        // source) is genuinely round-trippable, so return the uuid.
        return mAutoSaveUuid;
    }
    if (mCurrentSource->kind != loglib::LogConfiguration::Source::Kind::File)
    {
        // NetworkStream / future non-File kinds: the snapshot's
        // locator is a producer URI, not something
        // `StartStreamingOpenQueue` can re-bind on launch. Filtering
        // here prevents a fan-restore loop in which a legacy stream
        // entry would otherwise come back on every launch with a
        // "must re-bind manually" popup.
        return {};
    }
    return mAutoSaveUuid;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Final flush so the close-to-restore loop captures the exact
    // state the user left the window in (any column moves / filter
    // tweaks made after the last `streamingFinished`). Best-effort:
    // any I/O failure inside the manager is silenced and the close
    // proceeds, since blocking shutdown on disk errors would be a
    // worse user experience than losing one snapshot.
    //
    // `publishOpenWindow=false` so the flush does not add this uuid
    // to `openWindowsAtQuit` only for the very next line to remove
    // it -- saves two QSettings round-trips. `AutoSaveSessionSnapshot`
    // consults `mLastTerminalSessionMode` rather than the live
    // `mSessionMode`, so a closeEvent after a live-tail or network
    // stream finished does not write a phantom Recent Sessions entry
    // for an un-restorable session.
    AutoSaveSessionSnapshot(/*publishOpenWindow=*/false);
    // `DetachAutoSaveUuid` both removes the uuid from the persisted
    // open-windows set *and* clears `mAutoSaveUuid` on this window.
    // The clear is load-bearing: the stack-allocated primary window
    // in `main.cpp` is not `WA_DeleteOnClose`, so it stays in
    // `QApplication::topLevelWidgets()` past `closeEvent`. Without
    // the clear, the `aboutToQuit` handler's
    // `RestorableActiveSessionUuid` capture would re-publish the
    // uuid into `openWindowsAtQuit` and the next launch would
    // re-open the window the user just explicitly exited.
    DetachAutoSaveUuid();
    QMainWindow::closeEvent(event);
}

void MainWindow::SaveConfiguration()
{
    const QString file =
        QFileDialog::getSaveFileName(this, "Save Configuration", QString(), "JSON (*.json);;All Files (*)");
    if (file.isEmpty())
    {
        return;
    }
    try
    {
        DoSaveConfiguration(file, loglib::SaveScope::ColumnsOnly);
    }
    catch (std::exception &e)
    {
        QMessageBox::warning(this, "Error Saving Configuration", e.what());
    }
}

void MainWindow::SaveSession()
{
    const QString file = QFileDialog::getSaveFileName(this, "Save Session", QString(), "JSON (*.json);;All Files (*)");
    if (file.isEmpty())
    {
        return;
    }
    try
    {
        DoSaveConfiguration(file, loglib::SaveScope::Full);
    }
    catch (std::exception &e)
    {
        QMessageBox::warning(this, "Error Saving Session", e.what());
    }
}

void MainWindow::DoSaveConfiguration(const QString &path, loglib::SaveScope scope)
{
    // Mirror unconditionally even though every mutation point already
    // keeps the configuration current -- documents intent and
    // protects against a future mutator that forgets to mirror.
    // `scope` selects which subset lands on disk; `Save` throws on
    // I/O failure (callers catch).
    MirrorSessionStateToConfiguration();
    mModel->ConfigurationManager().Save(path.toStdString(), scope);
}

void MainWindow::LoadConfiguration()
{
    const QString file =
        QFileDialog::getOpenFileName(this, "Load Configuration", QString(), "JSON (*.json);;All Files (*)");
    if (file.isEmpty())
    {
        return;
    }
    DoLoadConfiguration(file);
}

void MainWindow::ShowConfigurationDiagnostics()
{
    if (!mDiagnosticsDialog)
    {
        mDiagnosticsDialog = new ConfigurationDiagnosticsDialog(mModel, this);
        mDiagnosticsDialog->setAttribute(Qt::WA_DeleteOnClose, false);
        // Wire the row drill-down once; the dialog survives close.
        connect(
            mDiagnosticsDialog, &ConfigurationDiagnosticsDialog::editColumnRequested, this, &MainWindow::EditColumn
        );
    }
    mDiagnosticsDialog->Refresh();
    mDiagnosticsDialog->show();
    mDiagnosticsDialog->raise();
    mDiagnosticsDialog->activateWindow();
}

void MainWindow::EditColumn(int columnIndex)
{
    if (mModel == nullptr)
    {
        return;
    }
    const auto &columns = mModel->Configuration().columns;
    if (columnIndex < 0 || static_cast<size_t>(columnIndex) >= columns.size())
    {
        return;
    }
    ColumnEditor editor(mModel, columnIndex, this);
    if (editor.exec() == QDialog::Accepted)
    {
        // Re-push visibility to the header and refresh the
        // diagnostics summary; the editor already handled the
        // model-side state.
        ApplyColumnVisibility();
        UpdateDiagnosticsStatus();
        UpdateUi();
    }
}

void MainWindow::ShowColumnsManager()
{
    if (!mColumnsManagerDialog)
    {
        mColumnsManagerDialog = new ColumnsManagerDialog(mModel, this, this);
        mColumnsManagerDialog->setAttribute(Qt::WA_DeleteOnClose, false);
    }
    mColumnsManagerDialog->Refresh();
    mColumnsManagerDialog->show();
    mColumnsManagerDialog->raise();
    mColumnsManagerDialog->activateWindow();
}

namespace
{
/// Walk the filter + row-order proxy chain down to a source-model
/// row, or -1 if anything along the chain is invalid.
[[nodiscard]] int MapProxyIndexToSourceRow(
    const QModelIndex &proxyIndex, const QAbstractProxyModel *filter, const QAbstractProxyModel *rowOrder
)
{
    if (!proxyIndex.isValid() || filter == nullptr || rowOrder == nullptr)
    {
        return -1;
    }
    const QModelIndex midIndex = filter->mapToSource(proxyIndex);
    if (!midIndex.isValid())
    {
        return -1;
    }
    const QModelIndex sourceIndex = rowOrder->mapToSource(midIndex);
    if (!sourceIndex.isValid())
    {
        return -1;
    }
    return sourceIndex.row();
}
} // namespace

void MainWindow::ShowRecordDetailsForProxyIndex(const QModelIndex &proxyIndex)
{
    if (mRecordDetailDock == nullptr)
    {
        return;
    }
    const int sourceRow = MapProxyIndexToSourceRow(proxyIndex, mSortFilterProxyModel, mRowOrderProxyModel);
    if (sourceRow < 0)
    {
        return;
    }
    mRecordDetailDock->ShowSourceRow(sourceRow);
    // `isHidden()` probes the dock's own explicit-hide flag (the
    // ancestor `isVisible()` is also false until `show()` has been
    // called on the main window). The `isVisible()` guard on `this`
    // avoids a Qt 6.8.3 + offscreen-QPA crash where
    // `QDockWidget::setVisible(true)` dereferences uninitialised
    // QMainWindowLayout dock-area state when the host has never been
    // shown. Production callers always see a visible main window.
    if (mRecordDetailDock->isHidden() && isVisible())
    {
        mRecordDetailDock->setVisible(true);
    }
    mRecordDetailDock->raise();
}

void MainWindow::RebindRecordDetailSelectionTracking()
{
    if (mTableView == nullptr)
    {
        return;
    }
    const QItemSelectionModel *selectionModel = mTableView->selectionModel();
    if (selectionModel == nullptr)
    {
        return;
    }
    // Bind to a member slot (not a lambda) so `Qt::UniqueConnection`
    // can dedupe; Qt only deduplicates pointer-to-member targets.
    connect(
        selectionModel,
        &QItemSelectionModel::currentRowChanged,
        this,
        &MainWindow::UpdateRecordDetailsFromSelection,
        Qt::UniqueConnection
    );
}

void MainWindow::UpdateRecordDetailsFromSelection()
{
    // Skip the refresh when the dock can't be seen. The dock's own
    // `visibilityChanged` hook re-pins from the selection on resume,
    // so navigation history isn't lost.
    if (mRecordDetailDock == nullptr || !mRecordDetailDock->IsVisibleForRefresh())
    {
        return;
    }
    const QItemSelectionModel *selectionModel = mTableView->selectionModel();
    if (selectionModel == nullptr)
    {
        mRecordDetailDock->Clear();
        return;
    }
    const QModelIndex current = selectionModel->currentIndex();
    const int sourceRow = MapProxyIndexToSourceRow(current, mSortFilterProxyModel, mRowOrderProxyModel);
    if (sourceRow < 0)
    {
        mRecordDetailDock->Clear();
        return;
    }
    // Skip the rebuild when the dock is already pinned to this row.
    // Mainly relevant on dock re-show: the dock has already refreshed
    // against its persistent index, and the selection unchanged.
    if (mRecordDetailDock->CurrentSourceRow() == sourceRow)
    {
        return;
    }
    mRecordDetailDock->ShowSourceRow(sourceRow);
}

void MainWindow::OpenRecordDetailWindow(int sourceRow)
{
    if (mModel == nullptr || sourceRow < 0 || sourceRow >= mModel->rowCount())
    {
        return;
    }
    const RecordDetailContent snapshot = BuildRecordDetailContent(*mModel, sourceRow);
    if (!snapshot.valid)
    {
        return;
    }
    auto *window = new RecordDetailWindow(snapshot, this);
    // Key the tracker by the heap address (captured before
    // `destroyed` fires, while the pointer is still valid). Using
    // `QPointer` equality wouldn't work -- by the time `destroyed`
    // emits, Qt has already nulled every `QPointer` to the window.
    const auto trackerId = reinterpret_cast<quintptr>(window);
    TrackedSnapshotWindow entry;
    entry.window = window;
    // Save the connection so `~MainWindow` can disconnect just this
    // lambda before member containers go away.
    entry.destroyedConnection =
        connect(window, &QObject::destroyed, this, [this, trackerId]() { mRecordDetailWindows.remove(trackerId); });
    mRecordDetailWindows.insert(trackerId, entry);
    window->show();
    window->raise();
    window->activateWindow();
}

void MainWindow::UpdateDiagnosticsStatus()
{
    if (mDiagnosticsButton == nullptr)
    {
        return;
    }
    const int mismatched = ConfigurationDiagnosticsDialog::MismatchedColumnCount(*mModel);
    if (mismatched == 0)
    {
        mDiagnosticsButton->hide();
        mDiagnosticsButton->setText(QString());
        mDiagnosticsButton->setToolTip(QString());
        return;
    }
    const QString text = tr("%n column mismatch(es)", nullptr, mismatched);
    mDiagnosticsButton->setText(text);
    mDiagnosticsButton->setToolTip(tr("%1 column(s) have values that do not match the configured type. "
                                      "Click to open Configuration Diagnostics.")
                                       .arg(mismatched));
    mDiagnosticsButton->show();
}

bool MainWindow::DoLoadConfiguration(const QString &path)
{
    try
    {
        // Drop the previous session's recents pin: a `Load
        // Configuration...` / `Open with Configuration...` is a
        // session boundary, and we must not let the next AutoSave
        // silently overwrite an unrelated session's JSON in place.
        // The callers that *do* want to re-pin a uuid after the
        // load (`OpenRecentSession`, `RestoreLastSessionFromPath`)
        // explicitly set `mAutoSaveUuid` once this function returns.
        DetachAutoSaveUuid();

        // Drop proxy rules and the active sort before the model is
        // reset: both were built for the old column layout. The
        // runtime `mFilters` map is rebuilt below.
        mSortFilterProxyModel->SetFilterRules({});
        mTableView->sortByColumn(-1, Qt::AscendingOrder);

        mModel->Reset();
        mModel->ConfigurationManager().Load(path.toStdString());
        // `Load` rewrites the configuration without emitting any
        // model signal; the reset re-initialises the header section
        // count and the wired `modelReset -> ApplyColumnVisibility`
        // pushes the freshly-loaded `visible` flags.
        mModel->NotifyConfigurationReplaced();
        UpdateUi();

        // Restore the persisted sort *before*
        // `RebuildFiltersFromConfiguration` -- that helper re-mirrors
        // session state and would otherwise overwrite the loaded
        // sort with the cleared proxy sort. Columns-only files
        // default to the `-1` "no sort" sentinel.
        const auto loadedSort = mModel->Configuration().sort;
        if (loadedSort.columnIndex >= 0 && loadedSort.columnIndex < mModel->columnCount())
        {
            mTableView->sortByColumn(
                loadedSort.columnIndex, loadedSort.descending ? Qt::DescendingOrder : Qt::AscendingOrder
            );
        }

        // Mirror the loaded source descriptor so the next session
        // save round-trips it. Metadata only -- we do not auto-bind
        // the file (a foreign session would be hostile).
        mCurrentSource = mModel->Configuration().source;

        RebuildFiltersFromConfiguration();
        return true;
    }
    catch (std::exception &e)
    {
        QMessageBox::warning(this, "Error Parsing Configuration", e.what());
        return false;
    }
}

void MainWindow::RebuildFiltersFromConfiguration()
{
    // Copy the loaded vector out, wipe runtime + menu state, then
    // walk the copy. Re-entering `AddLogFilter` rebuilds `mFilters`,
    // the Filters menu, and the wire-format vector. UUIDs are GUI-
    // only and regenerated here.
    const std::vector<loglib::LogConfiguration::LogFilter> loadedFilters = mModel->Configuration().filters;

    ClearAllFilters();
#ifdef LOGAPP_BUILD_TESTING
    mLastDroppedFilterCountForTest = 0;
#endif

    const auto &columns = mModel->Configuration().columns;
    std::vector<FilterValidationFailure> dropped;
    for (const auto &saved : loadedFilters)
    {
        if (auto failure = ValidateFilterAgainstColumns(saved, columns))
        {
            dropped.push_back(std::move(*failure));
            continue;
        }
        // Defer mirror + rule rebuild; one trailing sync below.
        AddLogFilter(QUuid::createUuid().toString(), saved, /*deferSync=*/true);
    }
    MirrorSessionStateToConfiguration();
    UpdateFilters();

    if (!dropped.empty())
    {
        constexpr size_t MAX_SHOWN = 20;
        QString message = QString("%1 saved filter(s) were dropped because they no longer fit the column layout:\n\n")
                              .arg(dropped.size());
        const size_t shown = std::min(dropped.size(), MAX_SHOWN);
        for (size_t i = 0; i < shown; ++i)
        {
            // Branch on the reason, not `columnHeader.empty()`: a real
            // column may legitimately have an empty `header`, so an
            // empty-check would mis-render type mismatches against it
            // as "out of range".
            const QString header = (dropped[i].reason == FilterValidationReason::OutOfRangeRow)
                                       ? QStringLiteral("(out-of-range column)")
                                       : QString::fromStdString(dropped[i].columnHeader);
            message += QString("- column '%1' (row %2): %3\n")
                           .arg(header)
                           .arg(dropped[i].row)
                           .arg(FilterValidationReasonString(dropped[i].reason));
        }
        if (dropped.size() > MAX_SHOWN)
        {
            message += QString("... and %1 more.").arg(dropped.size() - MAX_SHOWN);
        }
        ShowDroppedFiltersDialog(static_cast<int>(dropped.size()), message);
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
    const QString &filterId,
    const std::optional<loglib::LogConfiguration::LogFilter> &filter,
    bool openEditor,
    int initialColumn
)
{
    if (mModel->rowCount() == 0)
    {
        // No rows means there are no columns for the editor to
        // bind against. Hint the user via the status bar instead
        // of silently doing nothing.
        if (openEditor)
        {
            statusBar()->showMessage(
                tr("Open a log file before adding or editing filters."), STATUS_BAR_MESSAGE_TIMEOUT_MS
            );
        }
        return;
    }

    // Drop saved filters that no longer fit the current columns
    // (e.g. a string filter against a column that auto-promoted to
    // enum). `ValidateFilterAgainstColumns` is the shared check;
    // this pre-guard adapts its result to the legacy status-bar UX.
    // The post-editor "missing payload" guards remain inline because
    // they need to delete the editor on failure.
    std::optional<loglib::LogConfiguration::LogFilter> resolvedFilter = filter;
    if (resolvedFilter.has_value())
    {
        const auto &columns = mModel->Configuration().columns;
        if (auto failure = ValidateFilterAgainstColumns(*resolvedFilter, columns))
        {
            switch (failure->reason)
            {
            case FilterValidationReason::EmptyEnumSelection:
                statusBar()->showMessage(
                    QString("Saved enumeration filter for '%1' had no values selected; ignoring")
                        .arg(QString::fromStdString(failure->columnHeader)),
                    STATUS_BAR_MESSAGE_TIMEOUT_MS
                );
                ClearFilter(filterId);
                return;
            case FilterValidationReason::TypeMismatch:
                ClearFilter(filterId);
                statusBar()->showMessage(
                    QString("Filter '%1' was removed because the column type changed")
                        .arg(QString::fromStdString(failure->columnHeader)),
                    STATUS_BAR_MESSAGE_TIMEOUT_MS
                );
                resolvedFilter.reset();
                if (!openEditor)
                {
                    return;
                }
                // Fall through: drop the stale filter but still open a
                // fresh editor so the user can re-pick for the new
                // type. Regression: `TestSavedStringFilterDroppedOnNowEnumColumn`.
                break;
            case FilterValidationReason::OutOfRangeRow:
                // Should not reach `AddFilter` in normal flow (the
                // load path validates separately, and the Edit menu
                // re-reads the live `mFilters`). Guard anyway: a stale
                // row would crash or mis-bind `FilterEditor::Load`.
                // Recovery shape mirrors `TypeMismatch`.
                ClearFilter(filterId);
                statusBar()->showMessage(
                    QString("Filter '%1' was removed because its column no longer exists").arg(filterId),
                    STATUS_BAR_MESSAGE_TIMEOUT_MS
                );
                resolvedFilter.reset();
                if (!openEditor)
                {
                    return;
                }
                break;
            case FilterValidationReason::MissingTimeRange:
            case FilterValidationReason::MissingNumericRange:
            case FilterValidationReason::MissingStringMatch:
            case FilterValidationReason::MissingBooleanSelection:
                // Missing-payload reasons fall through to the
                // post-editor inline guards below (they need to
                // delete the editor on failure). The load path
                // validates these separately and never reaches here.
                break;
            }
        }
    }

    if (!openEditor)
    {
        // Configuration-load path: filter is already in `mFilters`.
        return;
    }

    auto *filterEditor = new FilterEditor(*mModel, filterId, this);
    // Without explicit cleanup, every Add / Edit click leaks a
    // `FilterEditor` (parented to `this`) until window teardown.
    // `WA_DeleteOnClose` handles the X-button; `accept()` /
    // `reject()` only hide, so we wire `accepted` / `rejected` to
    // `deleteLater` so OK and Cancel also clean up. The explicit
    // `delete filterEditor` branches below cover early-exit
    // "missing payload" cases that fire before the editor is shown.
    filterEditor->setAttribute(Qt::WA_DeleteOnClose);
    connect(filterEditor, &QDialog::accepted, filterEditor, &QObject::deleteLater);
    connect(filterEditor, &QDialog::rejected, filterEditor, &QObject::deleteLater);
    connect(filterEditor, &FilterEditor::FilterSubmitted, this, &MainWindow::FilterSubmitted);
    connect(filterEditor, &FilterEditor::FilterTimeStampSubmitted, this, &MainWindow::FilterTimeStampSubmitted);
    connect(filterEditor, &FilterEditor::FilterEnumSubmitted, this, &MainWindow::FilterEnumSubmitted);
    connect(filterEditor, &FilterEditor::FilterNumericRangeSubmitted, this, &MainWindow::FilterNumericRangeSubmitted);
    connect(filterEditor, &FilterEditor::FilterBooleanSubmitted, this, &MainWindow::FilterBooleanSubmitted);
    // Preselect the clicked column for the header "Add filter on
    // ..." entry. The `Load(...)` calls below also set the row, so
    // only meaningful when no payload is being restored.
    if (!resolvedFilter.has_value() && initialColumn >= 0)
    {
        filterEditor->SetInitialColumn(initialColumn);
    }
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
            filterEditor->Load(
                resolvedFilter->row,
                static_cast<qint64>(*resolvedFilter->filterBegin),
                static_cast<qint64>(*resolvedFilter->filterEnd)
            );
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
        else if (resolvedFilter->type == loglib::LogConfiguration::LogFilter::Type::Number)
        {
            if (!resolvedFilter->filterMinValue.has_value() && !resolvedFilter->filterMaxValue.has_value())
            {
                statusBar()->showMessage(
                    QString("Filter '%1' was dropped because its numeric range is missing").arg(filterId),
                    STATUS_BAR_MESSAGE_TIMEOUT_MS
                );
                ClearFilter(filterId);
                delete filterEditor;
                return;
            }
            filterEditor->Load(resolvedFilter->row, resolvedFilter->filterMinValue, resolvedFilter->filterMaxValue);
        }
        else if (resolvedFilter->type == loglib::LogConfiguration::LogFilter::Type::Boolean)
        {
            const BooleanFilterSides sides = DecodeBooleanFilterSides(resolvedFilter->filterValues);
            if (!sides.includeTrue && !sides.includeFalse)
            {
                statusBar()->showMessage(
                    QString("Filter '%1' was dropped because no boolean side was selected").arg(filterId),
                    STATUS_BAR_MESSAGE_TIMEOUT_MS
                );
                ClearFilter(filterId);
                delete filterEditor;
                return;
            }
            filterEditor->Load(resolvedFilter->row, sides.includeTrue, sides.includeFalse);
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
    mFilterSubMenus.clear();
    MirrorSessionStateToConfiguration();
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

void MainWindow::ClearFilter(const QString &filterID, bool deferSync)
{
    mFilters.erase(filterID.toStdString());
    mFilterSubMenus.erase(filterID.toStdString());
    if (!deferSync)
    {
        MirrorSessionStateToConfiguration();
        UpdateFilters();
    }

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

    // Defer mirror + rule-rebuild; the upcoming `AddLogFilter` does
    // both in one pass instead of running them twice per submit
    // (pathological on large logs).
    ClearFilter(filterID, /*deferSync=*/true);

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

    ClearFilter(filterID, /*deferSync=*/true);

    loglib::LogConfiguration::LogFilter filter;
    filter.type = loglib::LogConfiguration::LogFilter::Type::Time;
    filter.row = row;
    filter.filterBegin = beginTimeStamp;
    filter.filterEnd = endTimeStamp;

    AddLogFilter(filterID, filter);
}

void MainWindow::FilterEnumSubmitted(const QString &filterID, int row, const QStringList &selectedValues)
{
    ClearFilter(filterID, /*deferSync=*/true);

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

void MainWindow::FilterNumericRangeSubmitted(
    const QString &filterID, int row, std::optional<double> minValue, std::optional<double> maxValue
)
{
    // Reject inverted ranges up front; otherwise the predicate would
    // silently hide every row. Mirrors the time-range / regex probes.
    if (minValue.has_value() && maxValue.has_value() && *minValue > *maxValue)
    {
        // Use the same formatting as `AddLogFilter`'s menu title so
        // the rejection message matches what the user typed byte-
        // for-byte. Default `arg(double)` precision-6 truncates
        // values like `12345.6789` to `12345.7`.
        const QLocale cLocale = QLocale::c();
        const QString minStr = cLocale.toString(*minValue, 'g', std::numeric_limits<double>::max_digits10);
        const QString maxStr = cLocale.toString(*maxValue, 'g', std::numeric_limits<double>::max_digits10);
        statusBar()->showMessage(
            QString("Numeric-range filter rejected: min (%1) is greater than max (%2)").arg(minStr, maxStr),
            STATUS_BAR_MESSAGE_TIMEOUT_MS
        );
        ClearFilter(filterID);
        return;
    }
    if (!minValue.has_value() && !maxValue.has_value())
    {
        statusBar()->showMessage(
            QString("Numeric-range filter rejected: both bounds are unbounded"), STATUS_BAR_MESSAGE_TIMEOUT_MS
        );
        ClearFilter(filterID);
        return;
    }

    ClearFilter(filterID, /*deferSync=*/true);

    loglib::LogConfiguration::LogFilter filter;
    filter.type = loglib::LogConfiguration::LogFilter::Type::Number;
    filter.row = row;
    filter.filterMinValue = minValue;
    filter.filterMaxValue = maxValue;

    AddLogFilter(filterID, filter);
}

void MainWindow::FilterBooleanSubmitted(const QString &filterID, int row, bool includeTrue, bool includeFalse)
{
    if (!includeTrue && !includeFalse)
    {
        // Empty selection would hide every row.
        statusBar()->showMessage(
            QString("Boolean filter rejected: neither true nor false selected"), STATUS_BAR_MESSAGE_TIMEOUT_MS
        );
        ClearFilter(filterID);
        return;
    }

    ClearFilter(filterID, /*deferSync=*/true);

    loglib::LogConfiguration::LogFilter filter;
    filter.type = loglib::LogConfiguration::LogFilter::Type::Boolean;
    filter.row = row;
    if (includeTrue)
    {
        filter.filterValues.emplace_back("true");
    }
    if (includeFalse)
    {
        filter.filterValues.emplace_back("false");
    }

    AddLogFilter(filterID, filter);
}

QString MainWindow::BuildFilterTitle(const loglib::LogConfiguration::LogFilter &filter) const
{
    // No `default:`: a new `LogFilter::Type` must trip `-Wswitch`
    // rather than silently fall through and deref a `nullopt`.
    // The Q_ASSERTs below catch un-validated inserts in debug;
    // every filter in `mFilters` is supposed to have a fully-
    // populated payload (enforced by `ValidateFilterAgainstColumns`).
    switch (filter.type)
    {
    case loglib::LogConfiguration::LogFilter::Type::Time:
        Q_ASSERT(filter.filterBegin.has_value() && filter.filterEnd.has_value());
        return QString::fromStdString(
            loglib::UtcMicrosecondsToDateTimeString(*filter.filterBegin) + " - " +
            loglib::UtcMicrosecondsToDateTimeString(*filter.filterEnd)
        );
    case loglib::LogConfiguration::LogFilter::Type::Enumeration:
    {
        Q_ASSERT(!filter.filterValues.empty());
        QStringList values;
        values.reserve(static_cast<qsizetype>(filter.filterValues.size()));
        for (const std::string &v : filter.filterValues)
        {
            values.append(QString::fromStdString(v));
        }
        return values.join(QStringLiteral(", "));
    }
    case loglib::LogConfiguration::LogFilter::Type::Number:
    {
        Q_ASSERT(filter.filterMinValue.has_value() || filter.filterMaxValue.has_value());
        // Same C-locale, max-digits10 formatting as
        // `FilterEditor::Load` so the menu title and reopened editor
        // bounds match byte-for-byte. Default precision-6 would
        // silently truncate values like `12345.6789`.
        const QLocale cLocale = QLocale::c();
        const QString minStr =
            filter.filterMinValue.has_value()
                ? cLocale.toString(*filter.filterMinValue, 'g', std::numeric_limits<double>::max_digits10)
                : QStringLiteral("-inf");
        const QString maxStr =
            filter.filterMaxValue.has_value()
                ? cLocale.toString(*filter.filterMaxValue, 'g', std::numeric_limits<double>::max_digits10)
                : QStringLiteral("+inf");
        return QStringLiteral("[%1, %2]").arg(minStr, maxStr);
    }
    case loglib::LogConfiguration::LogFilter::Type::Boolean:
    {
        // Canonicalise to "true, false" order regardless of how
        // `filter.filterValues` is laid out (the submit slot always
        // writes "true" first, but a hand-edited config might not).
        const BooleanFilterSides sides = DecodeBooleanFilterSides(filter.filterValues);
        Q_ASSERT(sides.includeTrue || sides.includeFalse);
        QStringList values;
        if (sides.includeTrue)
        {
            values.append(QStringLiteral("true"));
        }
        if (sides.includeFalse)
        {
            values.append(QStringLiteral("false"));
        }
        return values.join(QStringLiteral(", "));
    }
    case loglib::LogConfiguration::LogFilter::Type::String:
        Q_ASSERT(filter.filterString.has_value());
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
        return QString::fromStdString(*filter.filterString);
    }
    Q_ASSERT_X(false, "MainWindow::BuildFilterTitle", "unhandled LogFilter::Type");
    return {};
}

void MainWindow::AddLogFilter(const QString &id, const loglib::LogConfiguration::LogFilter &filter, bool deferSync)
{
    mFilters[id.toStdString()] = filter;
    if (!deferSync)
    {
        MirrorSessionStateToConfiguration();
        UpdateFilters();
    }

    const QString title = BuildFilterTitle(filter);

    QMenu *menuItem = ui->menuFilters->addMenu(title);
    menuItem->menuAction()->setData(QVariant(id));
    // Track the submenu pointer so tests can find it without going
    // through `QAction::menu()` or `qobject_cast<QMenu*>(child)` --
    // both fail under the Linux Release offscreen-QPA toolchain.
    mFilterSubMenus[id.toStdString()] = menuItem;

    const QAction *editAction = menuItem->addAction(tr("Edit"));
    // Capture only the id and re-resolve the live filter at trigger
    // time, so a column reorder between menu build and click still
    // targets the right row. Regression:
    // `TestEditFilterAfterColumnReorderUsesCurrentRow`.
    //
    // The lint suppression below covers `mFilters.find` / `LogFilter`
    // copy, which can technically throw. The body has no real source
    // of exceptions (no user input, no I/O), so wrapping it in
    // try/catch would be pure noise. Same applies to the matching
    // Edit lambda in `BuildHeaderContextMenu`.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    connect(editAction, &QAction::triggered, this, [this, id]() {
        const auto it = mFilters.find(id.toStdString());
        if (it == mFilters.end())
        {
            AddFilter(id);
            return;
        }
        AddFilter(id, it->second);
    });

    const QAction *removeAction = menuItem->addAction(tr("Remove"));
    connect(removeAction, &QAction::triggered, this, [this, id]() { ClearFilter(id); });
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
    // Level columns hold canonical names in `filter.filterValues` and
    // expand them to raw entries at predicate-build time. Dictionary
    // growth can surface entries matching a selected level, so treat
    // these as never fully resolved and rebuild on every `Grew`.
    const auto &columnsCfg = mModel->Configuration().columns;
    if (filter.row >= 0 && static_cast<size_t>(filter.row) < columnsCfg.size() &&
        columnsCfg[static_cast<size_t>(filter.row)].type == loglib::LogConfiguration::Type::Level)
    {
        return false;
    }
    return std::ranges::all_of(filter.filterValues, [dictionary](const std::string &value) {
        return dictionary->Find(value) != loglib::INVALID_ENUM_VALUE_ID;
    });
}

namespace
{

/// JIT-compile @p regex eagerly so each captured copy doesn't re-JIT
/// lazily on its first `match()`. The lazy compile is the real
/// thread-safety footgun: a `QRegularExpressionPrivate` shared via
/// implicit copy across threads would race on the first match.
///
/// `QRegularExpression` is implicitly shared (CoW), so
/// captured-by-value copies in the matcher lambdas alias the primed
/// private. `loglib::FilterAcceptedRows` runs the predicate from
/// `tbb::parallel_for` workers, so these matchers do cross threads.
/// `QRegularExpression::match()` is documented as thread-safe once
/// compiled (which this prime guarantees), so the CoW alias is safe.
void PrimeRegex(QRegularExpression &regex)
{
    (void)regex.match(QStringLiteral(""));
}

/// Materialise the haystack as `QString`, skipping the
/// `replace` + `simplified()` walks when the bytes are already
/// canonical. The QString allocation is unavoidable on the regex path
/// (Qt's regex engine is UTF-16), but `QString::fromUtf8` alone is
/// roughly a third the cost of the full conversion.
QString HaystackQStringFast(std::string_view bytes)
{
    if (LogModel::IsSingleLineAsciiTrim(bytes))
    {
        return QString::fromUtf8(bytes.data(), static_cast<qsizetype>(bytes.size()));
    }
    return LogModel::ConvertToSingleLineCompactQString(bytes);
}

/// Build a Qt-flavoured matcher for `CallbackStringRowPredicate`.
/// Captures the compiled regex / needle once so the inner loop avoids
/// per-row recompilation.
///
/// The haystack is normalised via
/// `LogModel::ConvertToSingleLineCompactQString` so filters match the
/// same single-line text the user sees (and that Find applies). The
/// needle is left as-typed -- matches pre-`RowPredicate` `SortRole`
/// semantics so a needle with consecutive spaces stays a non-match
/// against a simplified haystack.
///
/// `Exactly` / `Contains` get an ASCII fast path: when both pattern
/// and haystack pass `LogModel::IsSingleLineAsciiTrim` (most log
/// lines), the matcher byte-compares / byte-searches directly and
/// skips the `QString::fromUtf8` + `simplified` round-trip. The
/// pattern is checked once at matcher construction so the per-row
/// cost is one early-exit walk plus the compare. Regex / wildcard
/// still need a QString (Qt's engine is UTF-16) but skip the
/// `replace` + `simplified` passes when the haystack is canonical.
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
        const QByteArray patternUtf8 = pattern.toUtf8();
        std::string patternBytes{patternUtf8.constData(), static_cast<size_t>(patternUtf8.size())};
        if (LogModel::IsSingleLineAsciiTrim(patternBytes))
        {
            // NOLINTNEXTLINE(bugprone-exception-escape)
            return [patternBytes = std::move(patternBytes), pattern](std::string_view bytes) {
                if (LogModel::IsSingleLineAsciiTrim(bytes))
                {
                    return bytes == std::string_view{patternBytes};
                }
                return LogModel::ConvertToSingleLineCompactQString(bytes) == pattern;
            };
        }
        // clang-tidy flags `QString::operator==` and the QString
        // allocation as exception-escape; both are benign here.
        // NOLINTNEXTLINE(bugprone-exception-escape)
        return
            [pattern](std::string_view bytes) { return LogModel::ConvertToSingleLineCompactQString(bytes) == pattern; };
    }
    case Match::Contains:
    {
        const QByteArray patternUtf8 = pattern.toUtf8();
        std::string patternBytes{patternUtf8.constData(), static_cast<size_t>(patternUtf8.size())};
        if (LogModel::IsSingleLineAsciiTrim(patternBytes))
        {
            // NOLINTNEXTLINE(bugprone-exception-escape)
            return [patternBytes = std::move(patternBytes), pattern](std::string_view bytes) {
                if (LogModel::IsSingleLineAsciiTrim(bytes))
                {
                    return bytes.contains(patternBytes);
                }
                return LogModel::ConvertToSingleLineCompactQString(bytes).contains(pattern);
            };
        }
        // NOLINTNEXTLINE(bugprone-exception-escape)
        return [pattern](std::string_view bytes) {
            return LogModel::ConvertToSingleLineCompactQString(bytes).contains(pattern);
        };
    }
    case Match::RegularExpression:
    {
        QRegularExpression regex(pattern);
        PrimeRegex(regex);
        return [regex](std::string_view bytes) { return regex.match(HaystackQStringFast(bytes)).hasMatch(); };
    }
    case Match::Wildcard:
    {
        QRegularExpression regex(QRegularExpression::wildcardToRegularExpression(pattern));
        PrimeRegex(regex);
        return [regex](std::string_view bytes) { return regex.match(HaystackQStringFast(bytes)).hasMatch(); };
    }
    }
    return [](std::string_view) { return false; };
}

} // namespace

void MainWindow::UpdateFilters()
{
    // Sort filters cheapest-first so `std::ranges::all_of` short-
    // circuits on the cheapest rejecting test:
    //   1. BoolRowPredicate           - GetValue + alternative test
    //   2. EnumRowPredicate           - GetEnumValueId + bitset test
    //   3. TimeRangeRowPredicate      - GetValue + int compare
    //   4. NumericRangeRowPredicate   - GetValue + double compare
    //   5. CallbackStringRowPredicate - regex / UTF-8 walk
    // Bool and Enum are effectively tied; Bool wins under the
    // assumption that bool columns reject more aggressively in
    // practice. Tie-break on column index for deterministic ordering.
    using LogFilterType = loglib::LogConfiguration::LogFilter::Type;
    auto costOf = [](LogFilterType t) -> int {
        switch (t)
        {
        case LogFilterType::Boolean:
            return 0;
        case LogFilterType::Enumeration:
            return 1;
        case LogFilterType::Time:
            return 2;
        case LogFilterType::Number:
            return 3;
        case LogFilterType::String:
        default:
            return 4;
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
            // `FilterTimeStampSubmitted` populates both bounds before the
            // filter ever reaches `mFilters`, and the switch case pins
            // `type == Time`, so the optionals are engaged here.
            rules.emplace_back(
                std::in_place_type<loglib::TimeRangeRowPredicate>,
                column,
                *filter.filterBegin, // NOLINT(bugprone-unchecked-optional-access)
                *filter.filterEnd    // NOLINT(bugprone-unchecked-optional-access)
            );
            break;
        case LogFilterType::Enumeration:
        {
            // `filter` aliases `mFilters`, so the underlying strings
            // outlive the views. `EnumRowPredicate`'s constructor
            // copies/indexes them and keeps no reference back into the
            // span (pinned by the lifetime test in `test_log_filter.cpp`).
            //
            // Level columns reuse the same predicate: expand the saved
            // canonical names (`"Info"`, ...) to every raw dictionary
            // entry that maps to a selected level via the
            // `LevelRankCache`. `expandedStorage` owns the strings
            // because dictionary `Resolve` returns views.
            std::vector<std::string_view> selectedViews;
            std::vector<std::string> expandedStorage;
            const auto &columnsCfg = mModel->Configuration().columns;
            const bool isLevelColumn =
                static_cast<size_t>(filter.row) < columnsCfg.size() &&
                columnsCfg[static_cast<size_t>(filter.row)].type == loglib::LogConfiguration::Type::Level;
            if (isLevelColumn)
            {
                const auto &lvlColumn = columnsCfg[static_cast<size_t>(filter.row)];
                const std::vector<loglib::LogLevel> *ranks =
                    mModel->Table().LevelRankCache(static_cast<size_t>(filter.row));
                const loglib::EnumDictionary *dictionary = ResolveEnumDictionary(filter.row);
                if (ranks == nullptr || dictionary == nullptr)
                {
                    // `Type::Level` column with no data yet: skip the
                    // rule. `EnumFilterFullyResolved` returns false for
                    // Level filters, so the next `Grew` rebuild will
                    // install the predicate. Rejecting every row here
                    // would hide unrelated rows.
                    break;
                }
                // Use `ResolveLevel` (not `ParseLevelName`) so custom
                // aliases saved while the column was Enumeration --
                // and any `levelMapping` overrides -- still resolve.
                // Matches `FilterEditor::Load`.
                std::unordered_set<loglib::LogLevel> selectedLevels;
                selectedLevels.reserve(filter.filterValues.size());
                for (const std::string &name : filter.filterValues)
                {
                    if (auto level = loglib::ResolveLevel(name, lvlColumn.levelMapping); level.has_value())
                    {
                        selectedLevels.insert(*level);
                    }
                }
                expandedStorage.reserve(ranks->size());
                for (size_t valueId = 0; valueId < ranks->size(); ++valueId)
                {
                    if (selectedLevels.contains((*ranks)[valueId]))
                    {
                        const std::string_view bytes = dictionary->Resolve(static_cast<loglib::EnumValueId>(valueId));
                        expandedStorage.emplace_back(bytes);
                    }
                }
                selectedViews.reserve(expandedStorage.size());
                for (const std::string &v : expandedStorage)
                {
                    selectedViews.emplace_back(v);
                }
                // Empty `selectedViews` is legitimate (e.g. user
                // picked `Trace` but only `Info`/`Warn` slots exist).
                // Matches the enum branch: reject every row.
                rules.emplace_back(
                    std::in_place_type<loglib::EnumRowPredicate>,
                    column,
                    std::span<const std::string_view>(selectedViews),
                    dictionary
                );
                break;
            }
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
        case LogFilterType::Number:
            // `FilterNumericRangeSubmitted` rejects all-unbounded and
            // inverted ranges upstream; at least one bound is set here.
            rules.emplace_back(
                std::in_place_type<loglib::NumericRangeRowPredicate>,
                column,
                filter.filterMinValue,
                filter.filterMaxValue
            );
            break;
        case LogFilterType::Boolean:
        {
            // `FilterBooleanSubmitted` already rejects the all-off
            // case upstream. Case-insensitive decode tolerates
            // hand-edited configs (e.g. `"True"` / `"FALSE"`).
            const BooleanFilterSides sides = DecodeBooleanFilterSides(filter.filterValues);
            rules.emplace_back(
                std::in_place_type<loglib::BoolRowPredicate>, column, sides.includeTrue, sides.includeFalse
            );
            break;
        }
        case LogFilterType::String:
        default:
            // `FilterSubmitted` populates both fields before the filter
            // ever reaches `mFilters`, and the switch case pins
            // `type == String`, so the optionals are engaged here.
            rules.emplace_back(
                std::in_place_type<loglib::CallbackStringRowPredicate>,
                column,
                // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
                MakeStringMatcher(QString::fromStdString(*filter.filterString), *filter.matchType)
            );
            break;
        }
    }
    mSortFilterProxyModel->SetFilterRules(std::move(rules));
}

void MainWindow::OnHeaderSectionMoved(int logicalIndex, int oldVisualIndex, int newVisualIndex)
{
    if (mApplyingSectionMove)
    {
        // Re-entered by the visual-reset loop below; swallow.
        return;
    }
    const QHeaderView *header = mTableView->horizontalHeader();
    if (header == nullptr)
    {
        return;
    }
    // The slot only handles a drag against an identity-mapped
    // header (visual == logical for every section). Anything else
    // would fold a stale visual permutation into the move and
    // rotate the wrong source column. We restore identity at the
    // end of every move; assert in debug, recover in release.
    Q_ASSERT_X(
        oldVisualIndex == logicalIndex,
        "MainWindow::OnHeaderSectionMoved",
        "header expected to be visual==logical before each drag"
    );
    if (oldVisualIndex != logicalIndex)
    {
        // Drop the drag, force identity, re-apply visibility, and
        // dump the full permutation so a recurrence in the wild is
        // at least diagnosable.
        QStringList permutation;
        const int sectionCount = header->count();
        permutation.reserve(sectionCount);
        for (int logical = 0; logical < sectionCount; ++logical)
        {
            permutation.append(QStringLiteral("%1->%2").arg(logical).arg(header->visualIndex(logical)));
        }
        qWarning() << "MainWindow::OnHeaderSectionMoved: header was not identity-mapped"
                   << "(logicalIndex=" << logicalIndex << ", oldVisualIndex=" << oldVisualIndex
                   << ", newVisualIndex=" << newVisualIndex
                   << ", logical->visual=" << permutation.join(QLatin1Char(',')) << "); resetting and ignoring drag.";
        statusBar()->showMessage(tr("Couldn't apply column move; please try again."), STATUS_BAR_MESSAGE_TIMEOUT_MS);
        ResetHeaderToIdentity();
        ApplyColumnVisibility();
        return;
    }
    const auto &columns = mModel->Configuration().columns;
    if (logicalIndex < 0 || static_cast<size_t>(logicalIndex) >= columns.size())
    {
        return;
    }
    // Identity-mapped header, so `newVisualIndex` is the absolute
    // final position the column should land at.
    const int dest = newVisualIndex;
    const int src = logicalIndex;
    if (src == dest)
    {
        return;
    }

    mApplyingSectionMove = true;
    // RAII reset: a latched guard would silently disable every
    // subsequent header drag if anything below threw.
    const auto guard = qScopeGuard([this]() { mApplyingSectionMove = false; });

    // The slot runs from the Qt event loop, where an unhandled
    // exception is UB. Wrap so a throw leaves the UI in a known
    // baseline rather than tearing the app down.
    try
    {
        // `MoveColumn` emits `columnsMoved` synchronously; the
        // connected `OnSourceColumnsMoved` slot remaps `mFilters`,
        // re-applies visibility, and refreshes the proxy rules.
        (void)mModel->MoveColumn(src, dest);

        // Qt usually restores visual == logical for shifted columns
        // by itself, but reset defensively so the next drag starts
        // from a known baseline regardless of Qt-version drift.
        // Re-entry is swallowed by `mApplyingSectionMove`.
        ResetHeaderToIdentity();
    }
    catch (const std::exception &e)
    {
        qWarning() << "MainWindow::OnHeaderSectionMoved: exception while applying move:" << e.what();
        statusBar()->showMessage(
            tr("Failed to apply column move: %1").arg(QString::fromLocal8Bit(e.what())), STATUS_BAR_MESSAGE_TIMEOUT_MS
        );
        // Recover to a known baseline. `OnSourceColumnsMoved` only
        // fires on a committed move, so re-apply visibility here.
        ResetHeaderToIdentity();
        ApplyColumnVisibility();
    }
    catch (...)
    {
        qWarning() << "MainWindow::OnHeaderSectionMoved: unknown exception while applying move";
        statusBar()->showMessage(tr("Failed to apply column move."), STATUS_BAR_MESSAGE_TIMEOUT_MS);
        ResetHeaderToIdentity();
        ApplyColumnVisibility();
    }
}

void MainWindow::OnSourceColumnsMoved(
    const QModelIndex &parent, int first, int last, const QModelIndex &destParent, int destColumn
)
{
    // Flat table model, so nested-parent moves are nonsense. Qt
    // carries the parents for future-compat; ignore defensively.
    if (parent.isValid() || destParent.isValid())
    {
        return;
    }
    if (first != last)
    {
        // Both move paths (`LogModel::MoveColumn` and the streaming
        // timestamp bubble) move a single column. Multi-column moves
        // would need a deliberate redesign; bail safely.
        return;
    }
    const int src = first;
    // `columnsMoved`'s `destColumn` uses "insert before" semantics;
    // `RemapColumnIndexAfterMove` wants the absolute final position.
    const int finalDest = (destColumn > src) ? destColumn - 1 : destColumn;
    if (src == finalDest)
    {
        return;
    }
    // The lib-side `MoveColumn` already rotated
    // `mConfiguration.filters[*].row`. Replay the same permutation
    // onto the runtime `mFilters` so both stores agree, then rebuild
    // the proxy rules. Deliberately not mirroring back here: that
    // would clobber the lib-side wire-format with a snapshot of the
    // runtime map, which is wrong in the (test-only) case where the
    // wire-format was populated via a direct `mgr.Load` that bypassed
    // `MainWindow::RebuildFiltersFromConfiguration` -- the runtime
    // map is empty there and a mirror would silently delete the
    // loaded filters. The mirror is already invoked at every
    // `mFilters` mutation point and again on every `Save`, so the
    // two stores remain consistent across the application lifecycle
    // without one being needed here.
    bool runtimeFilterChanged = false;
    for (auto &[id, filter] : mFilters)
    {
        const int remapped = loglib::LogConfigurationManager::RemapColumnIndexAfterMove(filter.row, src, finalDest);
        if (remapped != filter.row)
        {
            filter.row = remapped;
            runtimeFilterChanged = true;
        }
    }
    if (runtimeFilterChanged)
    {
        UpdateFilters();
    }

    // Re-apply hidden flags after the move. Qt usually carries them
    // through `columnsMoved`, but `initializeSections()` clears them
    // when the source has zero rows. Pinned by
    // `TestSourceColumnMovePreservesHiddenColumn`.
    ApplyColumnVisibility();
}

void MainWindow::ResetHeaderToIdentity()
{
    QHeaderView *header = mTableView->horizontalHeader();
    if (header == nullptr)
    {
        return;
    }
    const QSignalBlocker blocker(header);
    // Walk left-to-right and pin each logical index to its matching
    // visual position. Earlier iterations only touch later positions,
    // so the loop converges in one sweep.
    for (int target = 0; target < header->count(); ++target)
    {
        const int currentVisual = header->visualIndex(target);
        if (currentVisual != target)
        {
            header->moveSection(currentVisual, target);
        }
    }
#ifndef NDEBUG
    // Crash in debug if the loop failed to converge -- a soft
    // "ignore the drag" warning in `OnHeaderSectionMoved` would be
    // a much harder regression to trace.
    for (int logical = 0; logical < header->count(); ++logical)
    {
        Q_ASSERT_X(
            header->visualIndex(logical) == logical,
            "MainWindow::ResetHeaderToIdentity",
            "header is not identity-mapped after reset"
        );
    }
#endif
}

void MainWindow::ShowHeaderContextMenu(const QPoint &pos)
{
    QHeaderView *header = mTableView->horizontalHeader();
    if (header == nullptr)
    {
        return;
    }
    const int logical = header->logicalIndexAt(pos);
    if (logical < 0)
    {
        return;
    }
    HeaderContextMenu built = BuildHeaderContextMenu(logical, header);
    if (built.menu == nullptr)
    {
        return;
    }
    built.menu->setAttribute(Qt::WA_DeleteOnClose);
    built.menu->popup(header->mapToGlobal(pos));
}

MainWindow::HeaderContextMenu MainWindow::BuildHeaderContextMenu(int logicalColumn, QWidget *parent)
{
    HeaderContextMenu result;
    const auto &columns = mModel->Configuration().columns;
    if (logicalColumn < 0 || static_cast<size_t>(logicalColumn) >= columns.size())
    {
        return result;
    }
    auto *menu = new QMenu(parent != nullptr ? parent : mTableView);
    result.menu = menu;

    // Capture stable `keys` rather than the logical index: a column
    // move while the menu is open would otherwise leave the action
    // pointing at the wrong column. `FindColumnIndexByKeys`
    // re-resolves at trigger time.
    const std::vector<std::string> &thisKeys = columns[static_cast<size_t>(logicalColumn)].keys;
    const auto &thisColumn = columns[static_cast<size_t>(logicalColumn)];

    // Only the clicked column's label is needed -- re-showing hidden
    // columns is handled by the `View` menu, not this context menu.
    const QString thisLabel = ColumnMenuLabel(static_cast<size_t>(logicalColumn));

    // Only offer Hide for visible columns. Production right-clicks
    // always hit a visible section; the test seam may pass a hidden
    // index, where Hide would be a confusing no-op.
    if (thisColumn.visible)
    {
        const QAction *hideAction = menu->addAction(tr("Hide \"%1\"").arg(thisLabel));
        connect(hideAction, &QAction::triggered, this, [this, keys = thisKeys]() {
            const int idx = FindColumnIndexByKeys(keys);
            if (idx >= 0)
            {
                SetColumnVisible(idx, false);
            }
        });
    }

    // "Edit column..." is available even on hidden columns so the
    // editor doubles as the way to bring one back. Re-resolution by
    // stable keys mirrors the Hide path above.
    const QAction *editColumnAction = menu->addAction(tr("Edit column \"%1\"…").arg(thisLabel));
    connect(editColumnAction, &QAction::triggered, this, [this, keys = thisKeys]() {
        const int idx = FindColumnIndexByKeys(keys);
        if (idx >= 0)
        {
            EditColumn(idx);
        }
    });

    // Filter block: `Add filter on "<col>"` plus a submenu per
    // existing filter on this column. Lambdas capture stable keys /
    // ids and re-resolve at trigger time, so a column reorder
    // between build and click still hits the right index.
    //
    // Add and Edit are gated on row count > 0 -- `AddFilter` bails
    // out with a status-bar hint otherwise, so leaving them enabled
    // would advertise a no-op. Remove stays enabled (dropping a
    // filter doesn't need rows).
    const bool modelHasRows = mModel->rowCount() > 0;

    // Hidden columns: skip Add-filter. Production can't right-click
    // them, but the test seam can, and `SetInitialColumn` refuses to
    // preselect a hidden column -- so the action would advertise a
    // column the editor wouldn't actually preselect.
    if (thisColumn.visible)
    {
        if (!menu->isEmpty())
        {
            menu->addSeparator();
        }
        QAction *addFilterAction = menu->addAction(tr("Add filter on \"%1\"…").arg(thisLabel));
        addFilterAction->setEnabled(modelHasRows);
        connect(addFilterAction, &QAction::triggered, this, [this, keys = thisKeys]() {
            const int idx = FindColumnIndexByKeys(keys);
            if (idx < 0)
            {
                return;
            }
            AddFilter(QUuid::createUuid().toString(), std::nullopt, /*openEditor=*/true, /*initialColumn=*/idx);
        });
    }

    // Existing filters on this column, sorted by display title.
    // `mFilters` is an unordered_map keyed by UUID, so without
    // sorting the menu order would be effectively random.
    struct FilterEntry
    {
        std::string id;
        QString title;
        loglib::LogConfiguration::LogFilter::Type type;
    };
    std::vector<FilterEntry> filtersForColumn;
    filtersForColumn.reserve(mFilters.size());
    for (const auto &entry : mFilters)
    {
        if (entry.second.row == logicalColumn)
        {
            filtersForColumn.push_back(
                {.id = entry.first, .title = BuildFilterTitle(entry.second), .type = entry.second.type}
            );
        }
    }
    std::sort(filtersForColumn.begin(), filtersForColumn.end(), [](const FilterEntry &a, const FilterEntry &b) {
        const int compare = a.title.localeAwareCompare(b.title);
        if (compare != 0)
        {
            return compare < 0;
        }
        // Tie-break: type first (so a String `true, false` and a
        // Boolean filter group together), then UUID for determinism.
        if (a.type != b.type)
        {
            return a.type < b.type;
        }
        return a.id < b.id;
    });
    for (const FilterEntry &entry : filtersForColumn)
    {
        const QString filterId = QString::fromStdString(entry.id);
        QMenu *filterSubMenu = menu->addMenu(entry.title);
        result.filterSubMenus[entry.id] = filterSubMenu;
        QAction *editAction = filterSubMenu->addAction(tr("Edit"));
        editAction->setEnabled(modelHasRows);
        // Same id-resolve-on-trigger pattern as the Filters-menu
        // Edit action; see `AddLogFilter` for the lint suppression.
        // NOLINTNEXTLINE(bugprone-exception-escape)
        connect(editAction, &QAction::triggered, this, [this, filterId]() {
            const auto it = mFilters.find(filterId.toStdString());
            if (it == mFilters.end())
            {
                AddFilter(filterId);
                return;
            }
            AddFilter(filterId, it->second);
        });
        const QAction *removeAction = filterSubMenu->addAction(tr("Remove"));
        connect(removeAction, &QAction::triggered, this, [this, filterId]() { ClearFilter(filterId); });
    }

    // Re-showing hidden columns is intentionally not offered here:
    // the `View` menu already covers it (and is the only escape
    // hatch when *every* column is hidden, since no header section
    // is left to right-click).
    return result;
}

void MainWindow::SetColumnVisible(int logicalIndex, bool visible)
{
    const auto &columns = mModel->Configuration().columns;
    if (logicalIndex < 0 || static_cast<size_t>(logicalIndex) >= columns.size())
    {
        return;
    }
    mModel->ConfigurationManager().SetColumnVisible(static_cast<size_t>(logicalIndex), visible);
    QHeaderView *header = mTableView->horizontalHeader();
    if (header == nullptr)
    {
        return;
    }
    header->setSectionHidden(logicalIndex, !visible);
    // Hiding the sorted-by column would leave the sort active with
    // no UI glyph to clear it; reset to the unsorted baseline.
    // Pinned by `TestHidingSortedColumnClearsSort`.
    if (!visible && header->isSortIndicatorShown() && header->sortIndicatorSection() == logicalIndex)
    {
        mTableView->sortByColumn(-1, Qt::AscendingOrder);
    }
}

void MainWindow::ApplyColumnVisibility()
{
    QHeaderView *header = mTableView->horizontalHeader();
    if (header == nullptr)
    {
        return;
    }
    const auto &columns = mModel->Configuration().columns;
    const size_t end = std::min(columns.size(), static_cast<size_t>(std::max(0, header->count())));
    for (size_t i = 0; i < end; ++i)
    {
        header->setSectionHidden(static_cast<int>(i), !columns[i].visible);
    }
}

void MainWindow::RebuildViewMenu()
{
    QMenu *viewMenu = ui->menuView;
    if (viewMenu == nullptr)
    {
        return;
    }
    viewMenu->clear();

    // Top entry so it stays reachable even when no columns exist.
    QAction *manageColumnsAction = viewMenu->addAction(tr("Manage columns\u2026"));
    manageColumnsAction->setObjectName(QStringLiteral("actionManageColumns"));
    connect(manageColumnsAction, &QAction::triggered, this, &MainWindow::ShowColumnsManager);

    // Always reachable: opens the dock from cold and re-opens it
    // after the user dismissed it via the title-bar X.
    viewMenu->addAction(ui->actionToggleRecordDetails);

    const auto &columns = mModel->Configuration().columns;
    if (columns.empty())
    {
        viewMenu->addSeparator();
        // Disabled placeholder so an empty View menu is not silent.
        QAction *placeholder = viewMenu->addAction(tr("(no columns yet)"));
        placeholder->setEnabled(false);
        return;
    }
    viewMenu->addSeparator();
    const std::vector<QString> labels = BuildAllColumnMenuLabels();
    for (size_t i = 0; i < columns.size(); ++i)
    {
        const QString &label = labels[i];
        QAction *action = viewMenu->addAction(label);
        action->setCheckable(true);
        action->setChecked(columns[i].visible);
        // Capture stable `keys` so the toggle still hits the right
        // column if a column move lands between show and trigger.
        connect(action, &QAction::toggled, this, [this, keys = columns[i].keys](bool on) {
            const int idx = FindColumnIndexByKeys(keys);
            if (idx >= 0)
            {
                SetColumnVisible(idx, on);
            }
        });
    }
}

QString MainWindow::ColumnMenuLabel(size_t columnIndex) const
{
    const auto &columns = mModel->Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return {};
    }
    // Disambiguate duplicate headers via `keys` (the stable id).
    // Compare in `std::string` to avoid per-iteration UTF-8 alloc;
    // short-circuit once a second match is found. For a full scan
    // over every column, prefer `BuildAllColumnMenuLabels`.
    const std::string &thisHeader = columns[columnIndex].header;
    int duplicates = 0;
    for (const auto &other : columns)
    {
        if (other.header == thisHeader)
        {
            if (++duplicates > 1)
            {
                break;
            }
        }
    }
    QString header = QString::fromStdString(thisHeader);
    if (duplicates <= 1)
    {
        return header;
    }
    QStringList keys;
    keys.reserve(static_cast<qsizetype>(columns[columnIndex].keys.size()));
    for (const std::string &k : columns[columnIndex].keys)
    {
        keys.append(QString::fromStdString(k));
    }
    // `|` (not `,`) because JSON keys can legally contain commas.
    return QStringLiteral("%1 [%2]").arg(std::move(header), keys.join(QLatin1Char('|')));
}

std::vector<QString> MainWindow::BuildAllColumnMenuLabels() const
{
    const auto &columns = mModel->Configuration().columns;
    std::vector<QString> labels;
    labels.reserve(columns.size());
    if (columns.empty())
    {
        return labels;
    }
    // Tally duplicate-header counts once, then look up per entry.
    // Whole helper is O(N) over the columns vector.
    std::unordered_map<std::string, int> headerCounts;
    headerCounts.reserve(columns.size());
    for (const auto &c : columns)
    {
        ++headerCounts[c.header];
    }
    for (const auto &c : columns)
    {
        QString header = QString::fromStdString(c.header);
        const auto it = headerCounts.find(c.header);
        const int count = (it != headerCounts.end()) ? it->second : 1;
        if (count <= 1)
        {
            labels.push_back(std::move(header));
            continue;
        }
        QStringList keys;
        keys.reserve(static_cast<qsizetype>(c.keys.size()));
        for (const std::string &k : c.keys)
        {
            keys.append(QString::fromStdString(k));
        }
        labels.push_back(QStringLiteral("%1 [%2]").arg(std::move(header), keys.join(QLatin1Char('|'))));
    }
    return labels;
}

int MainWindow::FindColumnIndexByKeys(const std::vector<std::string> &keys) const
{
    if (keys.empty())
    {
        return -1;
    }
    const auto &columns = mModel->Configuration().columns;
    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (columns[i].keys == keys)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}
