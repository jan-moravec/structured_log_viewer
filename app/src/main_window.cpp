#include "main_window.hpp"
#include "./ui_main_window.h"

#include "column_editor.hpp"
#include "columns_manager_dialog.hpp"
#include "configuration_diagnostics_dialog.hpp"
#include "filter_editor.hpp"
#include "log_warning.hpp"
#include "network_stream_dialog.hpp"
#include "qt_streaming_log_sink.hpp"
#include "session_history_manager.hpp"
#include "streaming_control.hpp"
#include "theme_control.hpp"
#include "uuid_utils.hpp"

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
#include <loglib/theme.hpp>
#include <loglib/tcp_server_producer.hpp>
#include <loglib/udp_server_producer.hpp>

#include <QAbstractProxyModel>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
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

/// Format a millisecond-epoch timestamp as a "N time ago" string
/// for the Recent Sessions tooltip. Empty for non-positive
/// timestamps (legacy entries written before stamping was added).
/// Past 30 days, switches to an absolute local-time date.
QString FormatRelativeTimestamp(qint64 timestampMsEpoch, qint64 nowMs)
{
    if (timestampMsEpoch <= 0)
    {
        return {};
    }
    // Treat clock skew (timestamp in our future) as "just now".
    const qint64 diffMs = std::max<qint64>(nowMs - timestampMsEpoch, 0);
    constexpr qint64 SECOND_MS = 1000;
    constexpr qint64 MINUTE_MS = 60 * SECOND_MS;
    constexpr qint64 HOUR_MS = 60 * MINUTE_MS;
    constexpr qint64 DAY_MS = 24 * HOUR_MS;
    if (diffMs < MINUTE_MS)
    {
        return QStringLiteral("just now");
    }
    if (diffMs < HOUR_MS)
    {
        const qint64 minutes = diffMs / MINUTE_MS;
        return QStringLiteral("%1 %2 ago").arg(minutes).arg(minutes == 1 ? "minute" : "minutes");
    }
    if (diffMs < DAY_MS)
    {
        const qint64 hours = diffMs / HOUR_MS;
        return QStringLiteral("%1 %2 ago").arg(hours).arg(hours == 1 ? "hour" : "hours");
    }
    const qint64 days = diffMs / DAY_MS;
    constexpr qint64 RELATIVE_DAYS_CUTOFF = 30;
    if (days < RELATIVE_DAYS_CUTOFF)
    {
        return QStringLiteral("%1 %2 ago").arg(days).arg(days == 1 ? "day" : "days");
    }
    return QDateTime::fromMSecsSinceEpoch(timestampMsEpoch).toLocalTime().toString(QStringLiteral("yyyy-MM-dd"));
}

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

// "Is this file a configuration?" classifier used by
// `DispatchMixedOpenInput`. Two-step: cheap structural peek
// (BOM-aware leading `{` and a `"columns"` substring in the first
// 4 KB) followed by a Glaze parse of the bounded prefix. Rejects
// `columns.empty()` so `{}` (a valid default `LogConfiguration`)
// is not misclassified as a configuration.
///
/// Bounded read budget so a multi-gigabyte log starting with
/// `{"columns":...}` cannot freeze the GUI in the parse step.
constexpr qint64 CONFIG_PROBE_MAX_BYTES = 1024 * 1024;

bool FileLooksLikeConfiguration(const QString &file)
{
    if (file.isEmpty())
    {
        return false;
    }
    QFile probeFile(file);
    if (!probeFile.open(QIODevice::ReadOnly))
    {
        return false;
    }
    // Reject anything beyond the probe budget up front.
    if (probeFile.size() > CONFIG_PROBE_MAX_BYTES)
    {
        return false;
    }
    const QByteArray head = probeFile.read(4096);
    probeFile.seek(0);

    // Skip a UTF-8 BOM (EF BB BF) so the structural sniff sees the
    // first real payload byte.
    constexpr unsigned char UTF8_BOM_BYTE_0 = 0xEF;
    constexpr unsigned char UTF8_BOM_BYTE_1 = 0xBB;
    constexpr unsigned char UTF8_BOM_BYTE_2 = 0xBF;
    constexpr int UTF8_BOM_SIZE = 3;
    int cursor = 0;
    if (head.size() >= UTF8_BOM_SIZE && static_cast<unsigned char>(head[0]) == UTF8_BOM_BYTE_0 &&
        static_cast<unsigned char>(head[1]) == UTF8_BOM_BYTE_1 &&
        static_cast<unsigned char>(head[2]) == UTF8_BOM_BYTE_2)
    {
        cursor = UTF8_BOM_SIZE;
    }
    while (cursor < head.size())
    {
        const char c = head[cursor];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
        {
            break;
        }
        ++cursor;
    }
    const bool startsWithObject = cursor < head.size() && head[cursor] == '{';
    const bool mentionsColumns = head.contains("\"columns\"");
    if (!startsWithObject || !mentionsColumns)
    {
        return false;
    }

    // Read the bounded prefix.
    const QByteArray contentBytes = probeFile.read(CONFIG_PROBE_MAX_BYTES);
    probeFile.close();

    try
    {
        loglib::LogConfigurationManager probe;
        probe.LoadFromString(std::string_view(contentBytes.constData(), static_cast<size_t>(contentBytes.size())));
        return !probe.Configuration().columns.empty();
    }
    catch (...)
    {
        return false;
    }
}

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
        // At least one bound must be set; `nullopt` on the other side is
        // fed to the predicate as INT64_MIN / INT64_MAX at construction.
        if (!filter.filterBegin.has_value() && !filter.filterEnd.has_value())
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
    ApplyThemedWindowIcon();

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
    // Alternating row colours are intentionally disabled on the log
    // table: per-level theme colours already partition the rows
    // visually, and the additional light/dark stripe makes two rows
    // of the same level read as different (distracting). The
    // RecordDetailWidget / ColumnsManagerDialog tables -- both
    // level-less property lists -- still enable alternation as a
    // genuine reading aid.
    mTableView->setAlternatingRowColors(false);

    // Live theme refresh: the Preferences dialog and OS palette
    // changes both flow through `ThemeControl::themeChanged()`,
    // and we repaint the table + reapply chrome QSS in one slot.
    connect(&ThemeControl::Instance(), &ThemeControl::themeChanged, this, &MainWindow::OnThemeChanged);

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

    // Header stylesheet is owned by `ApplyTableStyleSheet` so theme
    // colours can layer onto the bold + padding rule.
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

    // Row-level context menu: inclusive time-range filter pinned to the
    // clicked row's timestamp. Installed on the table view itself, so
    // `pos` arrives in viewport coords (see `ShowRowContextMenu`).
    mTableView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(mTableView, &QWidget::customContextMenuRequested, this, &MainWindow::ShowRowContextMenu);
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
    // Disabled without a manager; `NewWindow` would no-op anyway,
    // but the menu state makes the affordance visible.
    ui->actionNewWindow->setEnabled(mHistoryManager != nullptr);
    connect(ui->actionOpen, &QAction::triggered, this, &MainWindow::OpenFiles);

    // Rebuild the Recent Sessions submenu on `aboutToShow` so
    // sibling-window mutations show up without us reacting to every
    // `changed()` signal.
    if (ui->menuRecentSessions != nullptr)
    {
        // QMenu hides per-action tooltips unless this is set.
        ui->menuRecentSessions->setToolTipsVisible(true);
        connect(ui->menuRecentSessions, &QMenu::aboutToShow, this, &MainWindow::RebuildRecentSessionsMenu);
    }
    connect(ui->actionOpenLogStream, &QAction::triggered, this, &MainWindow::OpenLogStream);
    connect(ui->actionOpenNetworkStream, &QAction::triggered, this, &MainWindow::OpenNetworkStream);
    connect(ui->actionSaveConfiguration, &QAction::triggered, this, &MainWindow::SaveConfiguration);
    connect(ui->actionSaveSession, &QAction::triggered, this, &MainWindow::SaveSession);
    connect(ui->actionLoadConfiguration, &QAction::triggered, this, &MainWindow::LoadConfiguration);
    // File -> Exit quits the whole application. `closeAllWindows`
    // fires `closeEvent` on every top-level so each window's
    // auto-save flush runs; the default `quitOnLastWindowClosed`
    // then triggers the `aboutToQuit` fan. We deliberately don't
    // also call `QApplication::quit()` so a window that vetoes its
    // close (`event->ignore()`) keeps the app alive.
    connect(ui->actionExit, &QAction::triggered, this, [] { QApplication::closeAllWindows(); });

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
    connect(mModel, &LogModel::streamingFinished, this, &MainWindow::OnStreamingFinished);
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
            // Re-entrancy guard: an inner `UpdateFilters` that
            // re-emits `enumColumnsChanged` must not rebuild on a
            // half-updated state. Queued signals that arrive after
            // the outer call returns rebuild normally.
            if (mApplyingEnumRebuild)
            {
                return;
            }
            mApplyingEnumRebuild = true;
            const auto guard = qScopeGuard([this]() { mApplyingEnumRebuild = false; });
            UpdateFilters();
        }
    });

    // Pull persisted streaming preferences on startup.
    StreamingControl::LoadConfiguration();
    ApplyStreamingRetention();
    ApplyDisplayOrder();

    // Timezone database initialisation lives in
    // `MainWindow::InitializeTimezoneDatabase`, called synchronously
    // from `main()` (and the QtTest fixture) before any window is
    // constructed. The constructor therefore stays free of
    // process-global side effects.
}

MainWindow::~MainWindow()
{
    // Defensive backstop in case any destruction path skipped
    // `closeEvent` (it normally runs first). Idempotent and cheap.
    DetachAutoSaveUuid();

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

bool MainWindow::InitializeTimezoneDatabase()
{
    // Idempotent: first successful call wins; subsequent calls are
    // no-ops with a single-shot diagnostic.
    static bool initialised = false;
    if (initialised)
    {
        return true;
    }

    // `qCritical` instead of a modal: the offscreen Qt plugin used
    // by CI deadlocks on `exec()`-style modals.
    std::vector<std::filesystem::path> searched;
    const auto tzdata = FindTzdata(searched);

    if (tzdata.empty())
    {
        qCritical().noquote() << "Fatal:" << FormatTzdataNotFoundMessage(searched);
        return false;
    }

    try
    {
        loglib::Initialize(tzdata);
    }
    catch (std::exception &e)
    {
        qCritical().noquote() << "Fatal: failed to initialize timezone database at"
                              << QString::fromStdString(tzdata.string()) << ":" << e.what();
        return false;
    }

    initialised = true;
    return true;
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

    QStringList files;
    files.reserve(urlList.size());
    for (const QUrl &url : urlList)
    {
        files.append(url.toLocalFile());
    }

    // Mirror `OpenFiles`: Shift forces Replace; default Appends
    // onto the active session. The dispatcher classifies each path
    // and routes mixed inputs through `DoLoadConfiguration` + Append.
    const bool forceReplace = event->modifiers().testFlag(Qt::ShiftModifier);
    DispatchMixedOpenInput(files, forceReplace ? OpenMode::Replace : OpenMode::Append);

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
    {
        // Skip during a self-induced apply: `ThemeControl::ApplyTheme`
        // calls `qApp->setPalette` (this event) and `qApp->setStyle`
        // (the case below) synchronously. The tail-end
        // `themeChanged` -> `OnThemeChanged` slot reapplies the
        // table QSS once for the whole apply, so the event
        // bounce-backs would just repeat the work.
        if (ThemeControl::IsApplyingTheme())
        {
            break;
        }
        // OS dark/light flip: re-evaluate Auto, which emits
        // `themeChanged` if the resolved theme changes (and
        // `OnThemeChanged` re-applies the table QSS as part of
        // that fan-out). When the resolved theme is unchanged --
        // Force mode, or Auto re-evaluating onto the same kind --
        // we still need to refresh the QSS because it encodes
        // palette-derived colours that the OS palette flip may
        // have shifted. Comparing the resolved name before / after
        // skips the redundant re-apply when `OnThemeChanged`
        // already ran.
        const QString priorName = QString::fromStdString(ThemeControl::Active().name);
        ThemeControl::Reevaluate();
        const QString currentName = QString::fromStdString(ThemeControl::Active().name);
        if (priorName == currentName)
        {
            ApplyTableStyleSheet();
        }
        break;
    }
    case QEvent::StyleChange:
        // Same guard reason as above -- self-induced StyleChange
        // from `qApp->setStyle` is covered by `OnThemeChanged`.
        if (ThemeControl::IsApplyingTheme())
        {
            break;
        }
        // External `qApp->setStyle` (none today, but defensive):
        // refresh the QSS so palette-derived colours follow.
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
        // No-history mode (test fixture / ad-hoc instance).
        return;
    }

    // Top-level peer with `WA_DeleteOnClose` so Qt owns lifetime.
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

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (const RecentSessionEntry &entry : entries)
    {
        QString label = entry.label;
        if (label.isEmpty())
        {
            label = entry.uuid;
        }
        QAction *action = ui->menuRecentSessions->addAction(label);
        // Tooltip: primary locator + file count + relative
        // timestamp so siblings with the same label stay
        // distinguishable.
        QString tooltip = entry.primaryLocator;
        if (entry.fileCount > 1)
        {
            tooltip += QStringLiteral(" (+ %1 more)").arg(entry.fileCount - 1);
        }
        const QString relativeTimestamp = FormatRelativeTimestamp(entry.timestampMsEpoch, nowMs);
        if (!relativeTimestamp.isEmpty())
        {
            if (!tooltip.isEmpty())
            {
                tooltip += QStringLiteral("\n");
            }
            tooltip += relativeTimestamp;
        }
        if (!tooltip.isEmpty())
        {
            action->setToolTip(tooltip);
        }
        const QString uuid = entry.uuid;
        connect(action, &QAction::triggered, this, [this, uuid]() { OpenRecentSession(uuid); });
    }

    ui->menuRecentSessions->addSeparator();
    const QAction *clearAction = ui->menuRecentSessions->addAction(QStringLiteral("Clear Recent Sessions"));
    // `menuRecentSessions->clear()` at the top of the next rebuild
    // deletes these QActions and severs the connections; no manual
    // cleanup needed.
    connect(clearAction, &QAction::triggered, this, [this]() {
        if (mHistoryManager != nullptr)
        {
            mHistoryManager->Clear();
            // "Clear history" wipes the store, not live sessions;
            // sibling windows will re-populate on their next save.
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
    // Always Append on the CLI / forward path so a user dragging
    // multiple files onto the binary in one go doesn't have each
    // clobber the previous. On empty-session start Append behaves
    // like a fresh open.
    const MixedInputResult result = DispatchMixedOpenInput(files, OpenMode::Append);

    // A lone-config argument applies columns / filters but never
    // streams rows, so surface a status-bar hint. We name the path
    // the dispatcher actually treated as the configuration (it may
    // not be `files.front()`).
    if (result.outcome == MixedInputDispatch::AppliedConfigOnly)
    {
        const QString message =
            tr("Loaded '%1' as a configuration. Open log files (File -> Open...) to populate the view.")
                .arg(result.appliedConfigPath);
        statusBar()->showMessage(message, STATUS_BAR_MESSAGE_TIMEOUT_MS);
        qInfo().noquote() << "OpenFilesForCli:" << message;
    }
    else if (result.outcome == MixedInputDispatch::AppliedConfigThenLogs)
    {
        // Same hint for the mixed branch -- helpful when the first
        // log file is large and rows take a moment to appear.
        const QString message =
            tr("Loaded '%1' as a configuration; streaming queued log files into it.").arg(result.appliedConfigPath);
        statusBar()->showMessage(message, STATUS_BAR_MESSAGE_TIMEOUT_MS);
        qInfo().noquote() << "OpenFilesForCli:" << message;
    }
}

void MainWindow::RestoreLastSessionFromPath(const QString &jsonPath)
{
    if (jsonPath.isEmpty() || !QFileInfo::exists(jsonPath))
    {
        return;
    }
    // Defensive reset: callers reusing a window would otherwise
    // carry a stale `LiveTail` into the restored session and trip
    // the live-tail guard in `ShouldAutoSaveSession` on the next
    // closeEvent.
    mLastTerminalSessionMode = SessionMode::Idle;
    if (!DoLoadConfiguration(jsonPath))
    {
        return;
    }

    // Pin the uuid before streaming so an OS-quit / crash between
    // here and the streaming-finished hook still restores this
    // window on next launch. The stem must parse as a QUuid AND
    // the file must live in the managed sessions dir; pinning an
    // external uuid-named JSON would silently fork it into a
    // managed copy on the next AutoSave.
    if (mHistoryManager != nullptr)
    {
        const QFileInfo info(jsonPath);
        const QString stem = info.completeBaseName();
        const QUuid parsed = QUuid::fromString(stem);
        const QDir managedDir = mHistoryManager->SessionsDir();
        const bool insideManagedDir = info.absoluteDir() == managedDir;
        if (!parsed.isNull() && insideManagedDir)
        {
            mAutoSaveUuid = stem;
            // Gate the publish on (a) `Touch` succeeding (index
            // still owns the stem) and (b)
            // `RestorableActiveSessionUuid()` non-empty (the
            // session can actually be reopened on next launch -- a
            // legacy NetworkStream entry would create a fan-restore
            // loop otherwise). The latch follows the bool return so
            // a contended / disabled publish doesn't claim a
            // publish that never happened.
            if (mHistoryManager->Touch(stem) && !RestorableActiveSessionUuid().isEmpty())
            {
                if (SessionHistoryManager::AddOpenWindowUuid(stem))
                {
                    mAutoSaveUuidPublished = true;
                }
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
        // Entry evicted (or backing JSON unlinked by a sibling)
        // between menu rebuild and click. Drop the dangling entry.
#ifdef LOGAPP_BUILD_TESTING
        if (!mSuppressDialogsForTest)
#endif
        {
            QMessageBox::warning(
                this,
                QStringLiteral("Recent Session Unavailable"),
                QStringLiteral("The JSON for this recent session has been removed. Dropping it from the list.")
            );
        }
        mHistoryManager->Remove(uuid);
        return;
    }

    // Pre-flight parse so a corrupt recents file doesn't destroy
    // the user's current view for nothing. The parsed value is
    // handed to `ApplyLoadedConfiguration` so the commit path
    // doesn't re-read the file (closing a TOCTOU window against a
    // sibling `Remove(uuid)`).
    loglib::LogConfiguration parsed;
    try
    {
        loglib::LogConfigurationManager probe;
        probe.Load(jsonPath.toStdString());
        parsed = probe.Configuration();
    }
    catch (const std::exception &e)
    {
#ifdef LOGAPP_BUILD_TESTING
        if (!mSuppressDialogsForTest)
#endif
        {
            QMessageBox::warning(
                this,
                QStringLiteral("Cannot Open Recent Session"),
                QStringLiteral("Failed to parse '%1':\n%2\n\nDropping this entry from Recent Sessions.")
                    .arg(jsonPath, QString::fromStdString(e.what()))
            );
        }
        // Drop the corrupt entry from the index; the on-disk JSON
        // will be reaped by `CleanupOrphanFiles` next launch.
        if (logapp::LooksLikeUuid(uuid))
        {
            mHistoryManager->Remove(uuid);
        }
        return;
    }

    // Recent Sessions is "open this exact view", so discard the
    // current session before applying. `NewSession` also detaches
    // our previous uuid; we re-pin below.
    NewSession();

    // Apply failure surfaces a `QMessageBox`; bail without queueing
    // files so the view is at least empty rather than mixed.
    if (!ApplyLoadedConfiguration(std::move(parsed)))
    {
        return;
    }

    // Pin the uuid before streaming. Publish gated by `Touch`
    // (index still owns @p uuid) and `RestorableActiveSessionUuid`
    // (loaded session is round-trippable -- legacy NetworkStream
    // snapshots would otherwise create a fan-restore loop). The
    // latch follows the bool return; see `RestoreLastSessionFromPath`.
    mAutoSaveUuid = uuid;
    if (mHistoryManager->Touch(uuid) && !RestorableActiveSessionUuid().isEmpty())
    {
        if (SessionHistoryManager::AddOpenWindowUuid(uuid))
        {
            mAutoSaveUuidPublished = true;
        }
    }

    // User-initiated click, so surface the non-File branch as a
    // `QMessageBox` rather than silently skipping.
    StreamFromCurrentSourceOrSkip(/*informIfNonFile=*/true);
}

void MainWindow::StreamFromCurrentSourceOrSkip(bool informIfNonFile)
{
    if (!loglib::HasLocators(mCurrentSource))
    {
        // Config has no source -- columns / filters are installed
        // but there's nothing to stream.
        return;
    }

    // `HasLocators` already gated `has_value`; clang-tidy's optional
    // analyser cannot trace through the helper.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto &source = *mCurrentSource;
    if (source.kind != loglib::LogConfiguration::Source::Kind::File)
    {
        // Legacy NetworkStream snapshots stored the producer URI as
        // a locator; we cannot reopen them, the user must re-bind
        // manually via "Open Network Stream...".
        if (informIfNonFile)
        {
#ifdef LOGAPP_BUILD_TESTING
            if (!mSuppressDialogsForTest)
#endif
            {
                QMessageBox::information(
                    this,
                    QStringLiteral("Network Stream Session"),
                    QStringLiteral("This recent session was a network stream; the columns and filters have been "
                                   "restored, but the producer must be re-bound manually via 'Open Network Stream...'.")
                );
            }
        }
        return;
    }

    QStringList files;
    files.reserve(static_cast<qsizetype>(source.locators.size()));
    for (const std::string &locator : source.locators)
    {
        files.append(QString::fromStdString(locator));
    }

    // Append mode so loaded filters survive into the streamed rows.
    // With the model empty, Append is non-destructive.
    StartStreamingOpenQueue(files, OpenMode::Append);
}

void MainWindow::NewSession()
{
    // Tear down all loaded state -- rows, filters, source, session
    // mode, columns, sort -- so the window matches "blank window"
    // semantics. `LogModel::Reset` handles producer stop + sink
    // drain for live-tail sessions, no extra branch needed.

    // Drop proxy state before the configuration wipe so no signal
    // handler can briefly evaluate against indices that become
    // dangling once `columns` is empty.
    mTableView->sortByColumn(-1, Qt::AscendingOrder);
    mSortFilterProxyModel->SetFilterRules({});

    // RAII latch so the synchronous `streamingFinished(Cancelled)`
    // emitted by `mModel->Reset()` doesn't run
    // `OnStreamingFinished` against the about-to-be-rebuilt session.
    const SessionSwitchScope switchGuard(*this);

    mModel->Reset();
    ClearAllFilters();

    // Wipe the configuration and re-emit `beginResetModel` /
    // `endResetModel` so the header collapses to zero sections.
    // The double reset (rows then header) is intentional.
    mModel->ConfigurationManager().Reset();
    mModel->NotifyConfigurationReplaced();

    mCurrentSource.reset();
    mSessionMode = SessionMode::Idle;
    mLastTerminalSessionMode = SessionMode::Idle;
    mStreamingFileName.clear();
    mStreamingLineCount = 0;
    mStreamingErrorCount = 0;
    mFirstStreamingBatchSeen = false;
    mSourceWaiting = false;
    // Drop the pinned uuid + open-windows membership so the next
    // AutoSave creates a fresh entry and a crash before then
    // doesn't re-restore the discarded session.
    DetachAutoSaveUuid();
    SetConfigurationUiEnabled(true);
    UpdateStreamToolbarVisibility();
    UpdateStreamingStatus();
    UpdateUi();
}

void MainWindow::OpenFiles()
{
    // Sample modifier state before the modal: `keyboardModifiers()`
    // after the dialog reports whatever is held *now*, almost never
    // what the user held on menu activation.
    const bool forceReplace = QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);

    const QStringList files = QFileDialog::getOpenFileNames(this, "Select Log Files", QString(), "All Files (*.*)");
    if (files.isEmpty())
    {
        return;
    }

    DispatchMixedOpenInput(files, forceReplace ? OpenMode::Replace : OpenMode::Append);
}

bool MainWindow::TryLoadAsConfiguration(const QString &file)
{
    // Probe via a throw-away manager so the live model is untouched
    // when the file is not actually a configuration. Reject
    // `columns.empty()` parses: `{}` and any session-only-fields
    // object would otherwise apply as a default `LogConfiguration`
    // and wipe the current column layout.
    try
    {
        loglib::LogConfigurationManager probe;
        probe.Load(file.toStdString());
        if (probe.Configuration().columns.empty())
        {
            return false;
        }
    }
    catch (...)
    {
        return false;
    }

    // Probe accepted: commit. A throw here is a TOCTOU race (file
    // changed between the two reads); we still report it as `false`
    // but live state may be partially mutated.
    try
    {
        // Drop proxy rules + sort before `Load` rewrites the
        // configuration so they don't evaluate against the old
        // column layout under the upcoming reset.
        mSortFilterProxyModel->SetFilterRules({});
        mTableView->sortByColumn(-1, Qt::AscendingOrder);

        mModel->ConfigurationManager().Load(file.toStdString());
        // Session boundary: drop the previous session's recents
        // pin so a later AutoSave cannot rewrite an unrelated
        // session's JSON under the stale uuid.
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

        // Mirror the loaded source so the next save round-trips it.
        // No auto-bind. Backfill `locatorDedupKeys` for JSON that
        // pre-dates the parallel-array schema split.
        mCurrentSource = mModel->Configuration().source;
        logapp::BackfillLocatorDedupKeys(mCurrentSource);

        RebuildFiltersFromConfiguration();
        return true;
    }
    catch (...)
    {
        return false;
    }
}

MainWindow::MixedInputResult MainWindow::DispatchMixedOpenInput(const QStringList &files, OpenMode logMode)
{
    if (files.isEmpty())
    {
        return MixedInputResult{.outcome = MixedInputDispatch::QueuedLogsOnly, .appliedConfigPath = QString()};
    }

    // Classify each file. The cheap 4 KB structural peek vetoes
    // typical JSONL logs before Glaze sees them. Empty strings are
    // filtered (CLI drops them but drag-drop / dialog don't).
    QStringList configPaths;
    QStringList logPaths;
    configPaths.reserve(files.size());
    logPaths.reserve(files.size());
    for (const QString &file : files)
    {
        if (file.isEmpty())
        {
            continue;
        }
        if (FileLooksLikeConfiguration(file))
        {
            configPaths.append(file);
        }
        else
        {
            logPaths.append(file);
        }
    }

    if (configPaths.size() >= 2)
    {
        // Multi-config rejection: stacking configurations would
        // silently lose all but the last. Bail without mutating
        // live state.
#ifdef LOGAPP_BUILD_TESTING
        if (!mSuppressDialogsForTest)
#endif
        {
            QMessageBox::warning(
                this,
                tr("Multiple Configurations Selected"),
                tr("Found %n configuration file(s) in the input. Drop or open exactly one configuration "
                   "file alongside your log files.\n\nConfigurations:\n%1",
                   nullptr,
                   static_cast<int>(configPaths.size()))
                    .arg(configPaths.join(QChar('\n')))
            );
        }
        return MixedInputResult{.outcome = MixedInputDispatch::RejectedMultiConfig, .appliedConfigPath = QString()};
    }

    if (configPaths.isEmpty())
    {
        StartStreamingOpenQueue(files, logMode);
        return MixedInputResult{.outcome = MixedInputDispatch::QueuedLogsOnly, .appliedConfigPath = QString()};
    }

    // Exactly one configuration in the input.
    const QString configPath = configPaths.front();
    if (logPaths.isEmpty())
    {
        // Lone-config: route through `TryLoadAsConfiguration` so
        // existing rows survive a config refresh. A TOCTOU failure
        // here surfaces as `QueuedLogsOnly` (nothing opened).
        if (TryLoadAsConfiguration(configPath))
        {
            UpdateUi();
            return MixedInputResult{.outcome = MixedInputDispatch::AppliedConfigOnly, .appliedConfigPath = configPath};
        }
        return MixedInputResult{.outcome = MixedInputDispatch::QueuedLogsOnly, .appliedConfigPath = QString()};
    }

    // Mixed: apply config with a full reset, then append the logs
    // so the loaded columns / filters / sort apply to the rows.
    if (!DoLoadConfiguration(configPath))
    {
        // TOCTOU: the file was rewritten between probe and commit.
        // The model was reset before the throw, so streaming logs
        // against the unintended default columns would mislead --
        // bail without queueing anything.
        return MixedInputResult{.outcome = MixedInputDispatch::QueuedLogsOnly, .appliedConfigPath = QString()};
    }
    StartStreamingOpenQueue(logPaths, OpenMode::Append);
    return MixedInputResult{.outcome = MixedInputDispatch::AppliedConfigThenLogs, .appliedConfigPath = configPath};
}

void MainWindow::StartStreamingOpenQueue(QStringList files, OpenMode mode)
{
    // Live-tail / network sessions are single-source: a new
    // static-files open implicitly tears them down regardless of
    // `mode`. Static sessions honour `mode`.
    const bool destructive = (mode == OpenMode::Replace) || (mSessionMode == SessionMode::LiveTail);

    if (destructive)
    {
        // `mModel->Reset()` synchronously stops any in-flight worker.
        mModel->Reset();
        ClearAllFilters();
        mCurrentSource.reset();
        mSessionMode = SessionMode::Idle;
        mLastTerminalSessionMode = SessionMode::Idle;
        DetachAutoSaveUuid();
    }
    else if (mModel->IsStreamingActive())
    {
        // Append onto an in-flight static session: queue and let
        // the existing `streamingFinished` -> `StreamNextPendingFile`
        // chain drain it. Starting another worker here would assert
        // in `LogModel::AppendStreaming`.
        mPendingOpenFiles.append(std::move(files));
        return;
    }
    else if (mSessionMode == SessionMode::Idle && mModel->rowCount() > 0)
    {
        // Append into a previously-finished static session: re-arm
        // `Static` so `StreamNextPendingFile` routes through
        // `AppendStreaming` instead of the row-clearing
        // `BeginStreaming` path.
        mSessionMode = SessionMode::Static;
    }
    // Otherwise (Idle + empty model): leave mode at Idle so the
    // first `StreamNextPendingFile` takes the `BeginStreaming` path,
    // which preserves runtime filters from a prior
    // "Load Configuration or Session...".

    mPendingOpenFiles = std::move(files);
    mPendingOpenErrors.clear();

    StreamNextPendingFile();
}

void MainWindow::OnStreamingFinished(StreamingResult result)
{
    // Skip outgoing-session UI cleanup when we're mid session-
    // switch (the synchronous `Cancelled` emitted by `mModel->Reset`
    // would otherwise flicker the wrong toolbar / status state at
    // the user). The outer caller finishes UI wiring itself.
    if (result == StreamingResult::Cancelled && mSessionSwitchInProgress)
    {
        return;
    }

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
    }
    else if (!mPendingOpenFiles.isEmpty())
    {
        mPendingOpenFiles.clear();
    }

    // Snapshot the mode before resetting so the auto-save gate
    // distinguishes static (worth saving) from live-tail / network
    // (transient). `mLastTerminalSessionMode` carries the same
    // value into a later closeEvent flush.
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

    // Auto-save on success so Recent Sessions + restore-on-launch
    // can reopen this view. `ShouldAutoSaveSession` filters out
    // non-restorable shapes (no manager, no source, streams,
    // live-tail).
    if (result == StreamingResult::Success && ShouldAutoSaveSession(justFinishedMode))
    {
        AutoSaveSessionSnapshot();
    }
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
        // Record every appended file in load order so SaveSession +
        // Recent Sessions can reopen the full set. We track two
        // forms per locator:
        //   - `displayPath`: original case, slash-normalised. The
        //     user-visible form (tooltip / title bar).
        //   - `dedupKey`: lower-cased on Windows; used for byte-
        //     equality dedup so two casings of the same Windows
        //     path collapse to one entry.
        // The worker opens the original `file`; only the persisted
        // descriptor sees the canonical forms.
        const std::string displayPath = logapp::CanonicalDisplayPath(file).toStdString();
        const std::string dedupKey = logapp::CanonicalLocator(file).toStdString();
        if (isFirstFileInSession)
        {
            mCurrentSource = loglib::LogConfiguration::Source{
                .kind = loglib::LogConfiguration::Source::Kind::File,
                .locators = {displayPath},
                .locatorDedupKeys = {dedupKey}
            };
        }
        else if (mCurrentSource.has_value() && mCurrentSource->kind == loglib::LogConfiguration::Source::Kind::File)
        {
            // Skip duplicates via the canonical dedup key; comparing
            // on the case-preserving `locators` would miss two
            // casings of the same Windows path.
            const bool alreadyPresent = std::any_of(
                mCurrentSource->locatorDedupKeys.begin(),
                mCurrentSource->locatorDedupKeys.end(),
                [&dedupKey](const std::string &existing) { return existing == dedupKey; }
            );
            if (!alreadyPresent)
            {
                loglib::AppendLocator(*mCurrentSource, displayPath, dedupKey);
            }
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

    // Surface and drop any queued multi-file-open continuation
    // before the AutoSave + destructive reset, so the user sees
    // their discarded selection explicitly rather than having the
    // shared cancel-handler silently clear `mPendingOpenFiles`.
    // Must run before AutoSave below: `MirrorSessionStateToConfiguration`
    // would otherwise union the never-opened paths into the prior
    // session's persisted locators.
    const int discardedQueuedFiles = static_cast<int>(mPendingOpenFiles.size());
    if (discardedQueuedFiles > 0)
    {
        statusBar()->showMessage(
            tr("Discarded %n queued file(s) before opening log stream.", nullptr, discardedQueuedFiles),
            STATUS_BAR_MESSAGE_TIMEOUT_MS
        );
        mPendingOpenFiles.clear();
    }

    // Flush the outgoing session so user edits made since its last
    // `streamingFinished` survive the destructive reset below.
    // No-op when there's nothing worth saving (live-tail / no uuid).
    // `publishOpenWindow=false` because we `DetachAutoSaveUuid()`
    // immediately afterwards.
    AutoSaveSessionSnapshot(/*publishOpenWindow=*/false);

    // RAII latch: see `NewSession` for why we need to suppress the
    // synchronous `Cancelled` cleanup.
    const SessionSwitchScope switchGuard(*this);

    mModel->Reset();
    ClearAllFilters();
    // Live-tail is transient and not auto-saved; leaving the prior
    // static session's uuid pinned would let closeEvent's
    // `RemoveOpenWindowUuid` drop that session from the multi-
    // window restore set even though the user only switched views.
    DetachAutoSaveUuid();

    mStreamingFileName = QFileInfo(file).fileName();
    // Live-tail single-file open: populate both arrays so the
    // parallel-array invariant holds across a future save.
    {
        const std::string displayPath = logapp::CanonicalDisplayPath(file).toStdString();
        const std::string dedupKey = logapp::CanonicalLocator(file).toStdString();
        mCurrentSource = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File,
            .locators = {displayPath},
            .locatorDedupKeys = {dedupKey}
        };
    }
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

    // Same rationale as `OpenLogStreamFromPath`: surface and drop
    // the pending queue before AutoSave + reset.
    const int discardedQueuedFiles = static_cast<int>(mPendingOpenFiles.size());
    if (discardedQueuedFiles > 0)
    {
        statusBar()->showMessage(
            tr("Discarded %n queued file(s) before opening network stream.", nullptr, discardedQueuedFiles),
            STATUS_BAR_MESSAGE_TIMEOUT_MS
        );
        mPendingOpenFiles.clear();
    }

    AutoSaveSessionSnapshot(/*publishOpenWindow=*/false);

    const SessionSwitchScope switchGuard(*this);

    mModel->Reset();
    ClearAllFilters();
    DetachAutoSaveUuid();

    mStreamingFileName = QString::fromStdString(displayName);
    // Network-stream locator is a producer URI, not a filesystem
    // path -- no canonicalisation applies, so dedup key == display.
    // Both arrays populated so the parallel-array invariant holds.
    mCurrentSource = loglib::LogConfiguration::Source{
        .kind = loglib::LogConfiguration::Source::Kind::NetworkStream,
        .locators = {displayName},
        .locatorDedupKeys = {displayName}
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
    // Header reorder + right-click are gated mid-stream because
    // `LogModel::MoveColumn` would race with `AppendKeys` mutating
    // `columns`. The View menu stays reachable (only flips `visible`).
    //
    // The row right-click menu is NOT gated: its only effect is
    // `AddLogFilter`, which doesn't race with the streaming pipeline,
    // and "narrow to newer logs" is a useful live-tail workflow.
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

QMenu *MainWindow::RecentSessionsMenu() const
{
    return ui->menuRecentSessions;
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
    // Test fixtures often skip the parallel `locatorDedupKeys`
    // array; backfill so downstream dedup loops behave correctly.
    logapp::BackfillLocatorDedupKeys(mCurrentSource);
}

void MainWindow::OpenFilesForTest(const QStringList &files, OpenMode mode)
{
    StartStreamingOpenQueue(files, mode);
}

MainWindow::MixedInputDispatch MainWindow::OpenMixedFilesForTest(const QStringList &files, OpenMode logMode)
{
    // Tests assert on the outcome enum directly. Code that needs
    // the applied config path can call `DispatchMixedOpenInput`.
    return DispatchMixedOpenInput(files, logMode).outcome;
}
#endif

void MainWindow::ApplyDisplayOrder()
{
    // Static -> static-mode preference; everything else -> stream-mode.
    const bool newestFirst = (mSessionMode == SessionMode::Static) ? StreamingControl::IsStaticNewestFirst()
                                                                   : StreamingControl::IsNewestFirst();

    mRowOrderProxyModel->SetReversed(newestFirst);

    mTableView->SetTailEdge(newestFirst ? LogTableView::TailEdge::Top : LogTableView::TailEdge::Bottom);

    // Note: we used to toggle `setAlternatingRowColors(!newestFirst)`
    // here to dodge the newest-first row-parity flicker (Qt keys
    // alternation off the visual row index). Alternation is now
    // unconditionally off on the log table -- per-level theme
    // colours already provide the reading aid and the stripe made
    // two rows of the same level read as different.

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
        // `(row, type)` head-on to avoid aliasing an `enum class`
        // value as an int reference; the tail uses `std::tie` for a
        // byte-identical lexicographic ordering.
        if (a.row != b.row)
        {
            return a.row < b.row;
        }
        if (a.type != b.type)
        {
            return static_cast<int>(a.type) < static_cast<int>(b.type);
        }
        return std::tie(
                   a.filterString,
                   a.matchType,
                   a.filterBegin,
                   a.filterEnd,
                   a.filterMinValue,
                   a.filterMaxValue,
                   a.filterValues
               ) <
               std::tie(
                   b.filterString,
                   b.matchType,
                   b.filterBegin,
                   b.filterEnd,
                   b.filterMinValue,
                   b.filterMaxValue,
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

    // Drop empty-locator Sources before mirroring: on-disk schema
    // omits `source` when nothing is bound, so a `Source{...,
    // locators: {}}` would round-trip as a label-less recents entry.
    //
    // Multi-file truncation fix: when `mPendingOpenFiles` is
    // non-empty, include both already-streamed and still-queued
    // locators so a quit mid-stream persists the full fan-out
    // (the next launch resumes the complete set rather than a
    // strict subset). Dedup via canonical keys.
    if (mCurrentSource.has_value() && mCurrentSource->kind == loglib::LogConfiguration::Source::Kind::File &&
        !mPendingOpenFiles.isEmpty())
    {
        loglib::LogConfiguration::Source mirrored = *mCurrentSource;
        // Seed `seen` with existing dedup keys (case-insensitive on
        // Windows) so pending duplicates of already-streamed paths
        // are skipped.
        std::unordered_set<std::string> seen;
        seen.reserve(mirrored.locatorDedupKeys.size() + static_cast<size_t>(mPendingOpenFiles.size()));
        for (const std::string &key : mirrored.locatorDedupKeys)
        {
            seen.insert(key);
        }
        for (const QString &pending : mPendingOpenFiles)
        {
            const std::string displayPath = logapp::CanonicalDisplayPath(pending).toStdString();
            const std::string dedupKey = logapp::CanonicalLocator(pending).toStdString();
            if (seen.insert(dedupKey).second)
            {
                loglib::AppendLocator(mirrored, displayPath, dedupKey);
            }
        }
        mModel->ConfigurationManager().SetSource(std::move(mirrored));
    }
    else if (loglib::HasLocators(mCurrentSource))
    {
        mModel->ConfigurationManager().SetSource(mCurrentSource);
    }
    else
    {
        mModel->ConfigurationManager().SetSource(std::nullopt);
    }

    // Invariant: either no source, or a source with at least one
    // locator. A `Source{kind: ..., locators: {}}` would round-trip
    // as a label-less recents entry.
    {
        const auto &mirrored = mModel->ConfigurationManager().Configuration().source;
        Q_ASSERT(!mirrored.has_value() || loglib::HasLocators(mirrored));
    }
}

bool MainWindow::ShouldAutoSaveSession(SessionMode justFinishedMode) const
{
    if (mHistoryManager == nullptr)
    {
        return false;
    }
    if (!loglib::HasLocators(mCurrentSource))
    {
        // No source -> can't be reopened from Recent Sessions.
        return false;
    }
    // `HasLocators` already gated `has_value`; clang-tidy's optional
    // analyser cannot trace through the helper.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto &source = *mCurrentSource;
    if (source.kind != loglib::LogConfiguration::Source::Kind::File)
    {
        // Network streams: locator is a producer URI, not a path.
        return false;
    }
    if (justFinishedMode == SessionMode::LiveTail)
    {
        // Live-tail looks like a static File source on disk but
        // binds a tailing producer. Reopening would silently
        // downgrade the user to a one-shot static load.
        return false;
    }
    return true;
}

void MainWindow::AutoSaveSessionSnapshot(bool publishOpenWindow)
{
    // Prefer live `mSessionMode`; fall back to the just-terminated
    // mode so a closeEvent firing after a live-tail finished still
    // hits the live-tail gate in `ShouldAutoSaveSession` (the live
    // field has already been reset to `Idle` by then).
    const SessionMode effectiveMode = (mSessionMode != SessionMode::Idle) ? mSessionMode : mLastTerminalSessionMode;
    if (!ShouldAutoSaveSession(effectiveMode))
    {
        return;
    }

    // Mirror live filters / sort / source so auto-save and the
    // user-driven `SaveSession` path produce the same JSON.
    MirrorSessionStateToConfiguration();

    const loglib::LogConfiguration &configuration = mModel->ConfigurationManager().Configuration();
    // `WriteSnapshotAndPublish` folds the snapshot + open-windows
    // publish under a single cross-process lock. `publishLanded`
    // tells us whether the publish half actually reached disk (it
    // doesn't on contention or when the `--new-instance` gate is
    // off); use it to drive the latch so retries stay coherent.
    bool publishLanded = false;
    const QString uuid = mHistoryManager->WriteSnapshotAndPublish(
        configuration, mAutoSaveUuid, /*publishOpenWindow=*/publishOpenWindow, &publishLanded
    );
    if (uuid.isEmpty())
    {
        // Save failed. The atomic temp+rename in
        // `LogConfigurationManager::Save` preserves any prior valid
        // `<uuid>.json`; the existing pins still point there.
        return;
    }
    // Pin so subsequent auto-saves rewrite the same JSON instead
    // of cluttering recents.
    mAutoSaveUuid = uuid;
    if (publishLanded)
    {
        mAutoSaveUuidPublished = true;
    }
}

void MainWindow::DetachAutoSaveUuid()
{
    if (mAutoSaveUuid.isEmpty())
    {
        return;
    }
    // Skip the cross-process Remove when we never published; saves
    // a lock acquisition on the common closeEvent path.
    if (mAutoSaveUuidPublished)
    {
        SessionHistoryManager::RemoveOpenWindowUuid(mAutoSaveUuid);
        mAutoSaveUuidPublished = false;
    }
    mAutoSaveUuid.clear();
}

QString MainWindow::RestorableActiveSessionUuid() const noexcept
{
    // Worth fan-restoring iff (a) a uuid is pinned, and (b) the
    // session is round-trippable. Mirrors `ShouldAutoSaveSession`
    // gates, with one difference: a pinned-uuid window with no
    // source (configuration-only restore) is still restorable
    // because the user explicitly clicked that recents entry.
    if (mAutoSaveUuid.isEmpty())
    {
        return {};
    }
    if (!loglib::HasLocators(mCurrentSource))
    {
        // Pinned uuid + no source = columns-only restore.
        return mAutoSaveUuid;
    }
    // `HasLocators` already gated `has_value`; clang-tidy's optional
    // analyser cannot trace through the helper.
    // NOLINTNEXTLINE(bugprone-unchecked-optional-access)
    const auto &source = *mCurrentSource;
    if (source.kind != loglib::LogConfiguration::Source::Kind::File)
    {
        // Stream sources cannot be re-bound from a saved locator.
        return {};
    }
    return mAutoSaveUuid;
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    // Run the base first so an `ignore()` from a subclass / event
    // filter aborts cleanly before we detach state.
    QMainWindow::closeEvent(event);
    if (!event->isAccepted())
    {
        return;
    }

    // Final flush so the restore-on-launch loop captures user
    // edits made after the last `streamingFinished`. Best-effort:
    // I/O failures inside the manager are silenced.
    //
    // `publishOpenWindow=false` because we Remove the uuid on the
    // next line anyway.
    AutoSaveSessionSnapshot(/*publishOpenWindow=*/false);
    // Detach + clear the uuid so the `aboutToQuit` fan doesn't
    // re-publish a window the user just closed (the primary stays
    // in `topLevelWidgets()` past closeEvent).
    DetachAutoSaveUuid();
    // Drop the rest of the session state so a subsequent
    // `aboutToQuit` flush short-circuits in `ShouldAutoSaveSession`
    // rather than minting a fresh uuid for the just-closed view.
    mCurrentSource.reset();
    mSessionMode = SessionMode::Idle;
    mLastTerminalSessionMode = SessionMode::Idle;
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
    // Parse into a temporary first so a parse failure cannot
    // destroy the current view (no TOCTOU window of two reads).
    loglib::LogConfiguration parsed;
    try
    {
        loglib::LogConfigurationManager probe;
        probe.Load(path.toStdString());
        parsed = probe.Configuration();
    }
    catch (std::exception &e)
    {
        QMessageBox::warning(this, "Error Parsing Configuration", e.what());
        return false;
    }
    return ApplyLoadedConfiguration(std::move(parsed));
}

bool MainWindow::ApplyLoadedConfiguration(loglib::LogConfiguration parsed)
{
    try
    {
        // Drop the previous session's pin before the destructive
        // clears. Callers that want to re-pin (`OpenRecentSession`,
        // `RestoreLastSessionFromPath`) do so after this returns.
        DetachAutoSaveUuid();

        // Drop proxy rules + sort before the model reset so they
        // don't briefly evaluate against the old column layout.
        mSortFilterProxyModel->SetFilterRules({});
        mTableView->sortByColumn(-1, Qt::AscendingOrder);

        // See `NewSession` for the session-switch latch rationale.
        const SessionSwitchScope switchGuard(*this);

        mModel->Reset();
        mModel->ConfigurationManager().SetConfiguration(std::move(parsed));
        // `SetConfiguration` does not emit a model signal; the
        // reset re-initialises the header section count and the
        // wired `modelReset` slot pushes the loaded `visible` flags.
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

        // Mirror the loaded source so the next save round-trips
        // it; no auto-bind (foreign sessions would be hostile).
        // Backfill the parallel dedup-keys array for older JSON.
        mCurrentSource = mModel->Configuration().source;
        logapp::BackfillLocatorDedupKeys(mCurrentSource);

        RebuildFiltersFromConfiguration();
        return true;
    }
    catch (std::exception &e)
    {
        // Reset already wiped the view; leave it empty and surface
        // the diagnostic. The pre-flight parse in
        // `DoLoadConfiguration` catches the common case before
        // crossing this destructive boundary.
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
            // At least one bound must be set; the other side may be
            // `nullopt` (shown as "No begin/end limit" in the editor).
            if (!resolvedFilter->filterBegin.has_value() && !resolvedFilter->filterEnd.has_value())
            {
                statusBar()->showMessage(
                    QString("Filter '%1' was dropped because its time range is missing").arg(filterId),
                    STATUS_BAR_MESSAGE_TIMEOUT_MS
                );
                ClearFilter(filterId);
                delete filterEditor;
                return;
            }
            const std::optional<qint64> begin =
                resolvedFilter->filterBegin.has_value()
                    ? std::optional<qint64>{static_cast<qint64>(*resolvedFilter->filterBegin)}
                    : std::nullopt;
            const std::optional<qint64> end =
                resolvedFilter->filterEnd.has_value()
                    ? std::optional<qint64>{static_cast<qint64>(*resolvedFilter->filterEnd)}
                    : std::nullopt;
            filterEditor->Load(resolvedFilter->row, begin, end);
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

void MainWindow::FilterTimeStampSubmitted(
    const QString &filterID, int row, std::optional<qint64> beginTimeStamp, std::optional<qint64> endTimeStamp
)
{
    // `nullopt` means "unbounded" on that side. Both-nullopt would
    // match every row and is rejected up front; the predicate
    // substitutes INT64 sentinels for the open side at construction.
    if (!beginTimeStamp.has_value() && !endTimeStamp.has_value())
    {
        statusBar()->showMessage(
            QString("Time-range filter rejected: at least one bound (begin or end) must be set"),
            STATUS_BAR_MESSAGE_TIMEOUT_MS
        );
        ClearFilter(filterID);
        return;
    }
    // Inversion only matters when both sides are bounded.
    if (beginTimeStamp.has_value() && endTimeStamp.has_value() && *beginTimeStamp > *endTimeStamp)
    {
        statusBar()->showMessage(
            QString("Time-range filter rejected: begin (%1) is after end (%2)").arg(*beginTimeStamp).arg(*endTimeStamp),
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
    {
        // `nullopt` renders as "any" rather than formatting the INT64
        // sentinels (which produced absurd 294247 AD / 292277 BC dates).
        // Validation rejects both-nullopt upstream, but render it as
        // "any - any" rather than asserting so a hand-edited config
        // surfaces visibly instead of crashing in Debug.
        const std::string beginStr =
            filter.filterBegin.has_value() ? loglib::UtcMicrosecondsToDateTimeString(*filter.filterBegin) : "any";
        const std::string endStr =
            filter.filterEnd.has_value() ? loglib::UtcMicrosecondsToDateTimeString(*filter.filterEnd) : "any";
        return QString::fromStdString(beginStr + " - " + endStr);
    }
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

void MainWindow::OnThemeChanged()
{
    ApplyTableStyleSheet();
    ApplyThemedWindowIcon();

    // Just repaint the viewport: Qt re-queries `data()` for every
    // visible cell on the next paint event, picking up the new
    // per-level brushes / fonts. Rows outside the viewport are
    // re-queried lazily on scroll.
    //
    // Avoid emitting `dataChanged` across the whole model: that
    // signal has to propagate through the proxy chain
    // (`RowOrderProxyModel` -> `LogFilterModel`) and Qt's model
    // index plumbing walks the row range, which is expensive for
    // a multi-million-row table even though the actual repaint
    // would only touch the viewport.
    if (mTableView != nullptr)
    {
        mTableView->viewport()->update();
    }

    // Satellite widgets that cache palette-derived state at
    // construction or `SetContent` time need an explicit nudge.
    // Pure `QPalette` consumers (which most of these are) also
    // get the cascading `ApplicationPaletteChange` we just
    // triggered via `qApp->setPalette` in `ThemeControl::ApplyTheme`,
    // but the record-detail views explicitly stash
    // `QPalette::PlaceholderText` on table items, so the cached
    // brushes outlive a palette change.
    if (mRecordDetailDock != nullptr && mRecordDetailDock->Widget() != nullptr)
    {
        mRecordDetailDock->Widget()->RefreshPalette();
    }
    for (const auto &tracked : mRecordDetailWindows)
    {
        if (RecordDetailWindow *window = tracked.window.data(); window != nullptr)
        {
            window->RefreshPalette();
        }
    }
    if (mColumnsManagerDialog != nullptr)
    {
        mColumnsManagerDialog->RefreshPalette();
    }
}

void MainWindow::ApplyThemedWindowIcon()
{
    // Pick the icon variant by `Theme::ThemeKind`, not by sampling
    // the current palette: in Force mode the user explicitly opts
    // into a kind that may differ from the OS chrome, and the
    // chrome the icon renders against is driven by the active
    // theme rather than the system palette. `ThemeKind` is a
    // closed enum (`Light` / `Dark`), so the branch is exhaustive.
    const loglib::Theme &theme = ThemeControl::Active();
    const QString iconPath = (theme.kind == loglib::ThemeKind::Light) ? QStringLiteral(":/icon-black.png")
                                                                      : QStringLiteral(":/icon-white.png");
    setWindowIcon(QIcon(iconPath));
}

void MainWindow::ApplyTableStyleSheet()
{
    // The base / alternate / selection colours come from the
    // active `QPalette` (pushed by `ThemeControl::ApplyTheme`), so
    // the table-body stylesheet is currently always empty: Qt's
    // standard delegate already uses the palette for those and
    // gets the fast paint path that way. The only QSS we still
    // need is the header `padding` + `bold` rule (which Qt has no
    // palette role for) plus optional header colours from the
    // theme.
    const loglib::Theme &theme = ThemeControl::Active();

    const QString bodyRule;

    QString headerRule = QStringLiteral("QHeaderView::section { padding: 8px; font-weight: bold;");
    if (theme.table.headerBackground.has_value() && !theme.table.headerBackground->empty())
    {
        headerRule += QStringLiteral(" background-color: %1;").arg(QString::fromStdString(*theme.table.headerBackground));
    }
    if (theme.table.headerForeground.has_value() && !theme.table.headerForeground->empty())
    {
        headerRule += QStringLiteral(" color: %1;").arg(QString::fromStdString(*theme.table.headerForeground));
    }
    headerRule += QStringLiteral(" }");

    // Skip the redundant write when neither rule changed. An empty
    // `setStyleSheet` still triggers Qt's full style polish cascade,
    // so the guard matters even for the body. Default-constructed
    // `mLastBodyStyleSheet` is null and compares equal to an empty
    // `bodyRule`, so the first call doesn't pay a polish on the
    // common no-body-styling path either.
    if (bodyRule != mLastBodyStyleSheet)
    {
        mTableView->setStyleSheet(bodyRule);
        mLastBodyStyleSheet = bodyRule;
    }
    if (headerRule != mLastHeaderStyleSheet)
    {
        mTableView->horizontalHeader()->setStyleSheet(headerRule);
        mLastHeaderStyleSheet = headerRule;
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
            // `nullopt` = unbounded; `value_or` feeds INT64 sentinels so
            // the per-row visitor stays a simple `>=` / `<=` pair while
            // the title and FilterEditor keep `nullopt` as canonical.
            // Validation guarantees at least one side is bounded.
            rules.emplace_back(
                std::in_place_type<loglib::TimeRangeRowPredicate>,
                column,
                filter.filterBegin.value_or(std::numeric_limits<int64_t>::min()),
                filter.filterEnd.value_or(std::numeric_limits<int64_t>::max())
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

void MainWindow::ShowRowContextMenu(const QPoint &pos)
{
    if (mTableView == nullptr || mSortFilterProxyModel == nullptr || mRowOrderProxyModel == nullptr)
    {
        return;
    }
    const QModelIndex proxyIndex = mTableView->indexAt(pos);
    if (!proxyIndex.isValid())
    {
        return;
    }
    const int sourceRow = MapProxyIndexToSourceRow(proxyIndex, mSortFilterProxyModel, mRowOrderProxyModel);
    if (sourceRow < 0)
    {
        return;
    }
    QMenu *menu = BuildRowContextMenu(sourceRow, mTableView);
    if (menu == nullptr)
    {
        return;
    }
    menu->setAttribute(Qt::WA_DeleteOnClose);
    // `customContextMenuRequested` from a `QAbstractItemView` delivers
    // `pos` in viewport coords; map via `viewport()` so the popup lands
    // under the cursor.
    menu->popup(mTableView->viewport()->mapToGlobal(pos));
}

QMenu *MainWindow::BuildRowContextMenu(int sourceRow, QWidget *parent)
{
    if (mModel == nullptr || mModel->rowCount() <= 0 || sourceRow < 0 ||
        static_cast<size_t>(sourceRow) >= static_cast<size_t>(mModel->rowCount()))
    {
        return nullptr;
    }

    // Pin to the first time column (shared with the Record Details
    // summary via `FirstTimeColumnIndex`).
    const auto &config = mModel->Configuration();
    const auto &columns = config.columns;
    const int timeCol = loglib::FirstTimeColumnIndex(config);
    if (timeCol < 0)
    {
        return nullptr;
    }

    // `nullopt` for `monostate` and non-time-shaped slots: skip the
    // menu rather than advertise a no-op action.
    const std::optional<int64_t> micros = loglib::AsEpochMicroseconds(
        mModel->Table().GetValue(static_cast<size_t>(sourceRow), static_cast<size_t>(timeCol))
    );
    if (!micros.has_value())
    {
        return nullptr;
    }

    auto *menu = new QMenu(parent != nullptr ? parent : mTableView);

    // Capture the stable column keys (not the index) so the action
    // still targets the right column if a streaming reorder fires
    // between menu build and click.
    const std::vector<std::string> timeKeys = columns[static_cast<size_t>(timeCol)].keys;
    // `ColumnMenuLabel` appends `[key]` to disambiguate duplicate
    // headers, matching `BuildHeaderContextMenu`.
    const QString colLabel = ColumnMenuLabel(static_cast<size_t>(timeCol));
    const qint64 boundary = *micros;

    // Each action re-resolves the column by its captured keys at
    // trigger time, then dispatches a fresh-uuid time filter. Only
    // which side carries the bound varies; the open side uses
    // `nullopt` so the title shows "any" and the editor round-trips
    // it faithfully.
    //
    // `timeKeys` is captured by reference here (the local outlives
    // every synchronous call below), then copied into the connect
    // lambda which is invoked asynchronously.
    auto addRangeAction = [this, menu, &timeKeys, boundary](
                              const QString &label, std::optional<qint64> begin, std::optional<qint64> end
                          ) {
        const QAction *action = menu->addAction(label);
        // NOLINTNEXTLINE(bugprone-exception-escape)
        connect(action, &QAction::triggered, this, [this, timeKeys, begin, end]() {
            const int col = FindColumnIndexByKeys(timeKeys);
            if (col < 0)
            {
                return;
            }
            FilterTimeStampSubmitted(QUuid::createUuid().toString(), col, begin, end);
        });
    };

    addRangeAction(tr("Show only newer logs (%1)").arg(colLabel), boundary, std::nullopt);
    addRangeAction(tr("Show only older logs (%1)").arg(colLabel), std::nullopt, boundary);

    return menu;
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
