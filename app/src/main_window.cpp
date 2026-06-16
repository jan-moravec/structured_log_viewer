#include "main_window.hpp"
#include "./ui_main_window.h"

#include "column_editor.hpp"
#include "columns_manager_dialog.hpp"
#include "configuration_diagnostics_dialog.hpp"
#include "filter_editor.hpp"
#include "icon_loader.hpp"
#include "level_cell_delegate.hpp"
#include "log_warning.hpp"
#include "network_stream_dialog.hpp"
#include "qt_streaming_log_sink.hpp"
#include "session_history_manager.hpp"
#include "shortcuts_dialog.hpp"
#include "streaming_control.hpp"
#include "theme_control.hpp"
#include "uuid_utils.hpp"

#include <loglib/bytes_producer.hpp>
#include <loglib/enum_dictionary.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/internal/ascii_case.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_factory.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_processing.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/parsers/logfmt_parser.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/stream_line_source.hpp>
#include <loglib/tailing_bytes_producer.hpp>
#include <loglib/tcp_server_producer.hpp>
#include <loglib/theme.hpp>
#include <loglib/udp_server_producer.hpp>

#include <QAbstractProxyModel>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCollator>
#include <QCoreApplication>
#include <QDateTime>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
#include <QTableView>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUuid>
#include <QVBoxLayout>
#include <QVariant>

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

/// Construct the parser implementation matching @p format. Used by every
/// `BeginStreaming` / `AppendStreaming` open path so the parser choice
/// follows from the persisted `Source::format` rather than being
/// scattered across hard-coded `JsonParser` constructions.
std::unique_ptr<loglib::LogParser> MakeParserForFormat(loglib::LogConfiguration::Source::Format format)
{
    switch (format)
    {
    case loglib::LogConfiguration::Source::Format::Logfmt:
        return std::make_unique<loglib::LogfmtParser>();
    case loglib::LogConfiguration::Source::Format::Json:
        return std::make_unique<loglib::JsonParser>();
    }
    return std::make_unique<loglib::JsonParser>();
}

/// Probe @p file via every registered `LogFactory` parser and return
/// the first that accepts it. Mirrors the auto-detect ordering of
/// `loglib::ParseFile(path)` (JSON first, then logfmt) so the GUI
/// open-paths classify a file the same way the lib does. Falls back
/// to `Json` when no parser claims the file: the subsequent parse
/// will surface the bytes as parse errors, which is the desired
/// "first row tells the user what's wrong" UX.
loglib::LogConfiguration::Source::Format DetectFormatForPath(const std::filesystem::path &file)
{
    for (int i = 0; i < static_cast<int>(loglib::LogFactory::Parser::Count); ++i)
    {
        const auto parserType = static_cast<loglib::LogFactory::Parser>(i);
        const std::unique_ptr<loglib::LogParser> probe = loglib::LogFactory::Create(parserType);
        if (probe->IsValid(file))
        {
            switch (parserType)
            {
            case loglib::LogFactory::Parser::Logfmt:
                return loglib::LogConfiguration::Source::Format::Logfmt;
            case loglib::LogFactory::Parser::Json:
            case loglib::LogFactory::Parser::Count:
                return loglib::LogConfiguration::Source::Format::Json;
            }
        }
    }
    return loglib::LogConfiguration::Source::Format::Json;
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : MainWindow(nullptr, nullptr, parent)
{
}

MainWindow::MainWindow(ThemeControl *theme, QWidget *parent)
    : MainWindow(theme, nullptr, parent)
{
}

MainWindow::MainWindow(ThemeControl *theme, SessionHistoryManager *historyManager, QWidget *parent)
    : QMainWindow(parent), ui(new Ui::MainWindow), mHistoryManager(historyManager), mTheme(theme)
{
    ui->setupUi(this);
    UpdateWindowTitle();
    ApplyThemedWindowIcon();

    mTableView = new LogTableView(this);
    mLayout = new QVBoxLayout(ui->centralWidget);
    mLayout->addWidget(mTableView, 1);
    mTableView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    // Build before the model so it can wire anchor listeners.
    mAnchors = new AnchorManager(this);
    mModel = new LogModel(mTableView, mTheme, mAnchors);
    mTableView->setModel(mModel);
    mTableView->SetAnchorManager(mAnchors);

    mAnchorsDock = new AnchorsDock(mAnchors, mModel, mTheme, this);
    addDockWidget(Qt::RightDockWidgetArea, mAnchorsDock);
    mAnchorsDock->hide();
    connect(mAnchorsDock, &AnchorsDock::jumpToAnchorRequested, this, &MainWindow::SelectSourceRow);

    mActionToggleAnchors = new QAction(tr("Anchors"), this);
    mActionToggleAnchors->setObjectName(QStringLiteral("actionToggleAnchors"));
    mActionToggleAnchors->setCheckable(true);
    mActionToggleAnchors->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_K));
    addAction(mActionToggleAnchors);
    WireDockToggle(mAnchorsDock, mActionToggleAnchors, &AnchorsDock::closed);
    // `modelReset` clears the header's hidden flags, but
    // `Column::visible` survives. Re-apply on every reset so load /
    // re-stream / teardown all stay consistent with the saved config.
    connect(mModel, &QAbstractItemModel::modelReset, this, &MainWindow::ApplyColumnVisibility);
    mTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    // ExtendedSelection: plain click replaces, Ctrl toggles, Shift
    // extends a range, drag is contiguous (Explorer/Excel idiom).
    mTableView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mTableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    // Per-level theme colours already partition rows; an extra
    // alternation stripe would make two rows of the same level
    // read as different. Secondary tables (Record Details,
    // Columns Manager) keep alternation since they're plain
    // property lists.
    mTableView->setAlternatingRowColors(false);

    // Single entry point for both Preferences-driven and
    // OS-driven theme refreshes. Skipped in the no-theme test
    // fixture path; theme-dependent assertions wire the themed
    // overload of `MainWindow`.
    if (mTheme != nullptr)
    {
        connect(mTheme, &ThemeControl::themeChanged, this, &MainWindow::OnThemeChanged);
    }

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

    // Icon-pill delegate is created only when we have a
    // `ThemeControl`; the no-theme test fixture keeps the standard
    // text delegate (`IsLevelIconModeActive` is always false there).
    if (mTheme != nullptr)
    {
        mLevelCellDelegate = new LevelCellDelegate(mTheme, this);

        // Reapply on every signal that can change the first-level
        // column index. `columnsMoved` is handled inside
        // `OnSourceColumnsMoved` instead, so the filter-map remap
        // runs before the delegate reapply.
        connect(mModel, &QAbstractItemModel::modelReset, this, &MainWindow::ApplyLevelCellDelegate);
        connect(mModel, &QAbstractItemModel::columnsInserted, this, &MainWindow::ApplyLevelCellDelegate);
        connect(mModel, &QAbstractItemModel::columnsRemoved, this, &MainWindow::ApplyLevelCellDelegate);
        // Promote/Demote between Level and other enum types flips
        // which column is "the level column". `Grew` is a no-op
        // for our purposes (dict expansion doesn't move columns).
        connect(
            mModel,
            &LogModel::enumColumnsChanged,
            this,
            [this](EnumColumnsChangeReason reason, int /*columnIndex*/) {
                if (reason == EnumColumnsChangeReason::Grew)
                {
                    return;
                }
                ApplyLevelCellDelegate();
            }
        );

        ApplyLevelCellDelegate();
    }

    mTableView->resizeColumnsToContents();

    // Header stylesheet lives in `ApplyTableStyleSheet` so theme
    // colours can layer onto the bold + padding rule.
    mTableView->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    mTableView->horizontalHeader()->resizeSections(QHeaderView::Stretch);
    mTableView->horizontalHeader()->setStretchLastSection(true);
    mTableView->horizontalHeader()->setHighlightSections(false);
    mTableView->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    mTableView->horizontalHeader()->setSectionsMovable(true);
    mTableView->horizontalHeader()->setContextMenuPolicy(Qt::CustomContextMenu);
    // Cycle Asc -> Desc -> none. The "no sort" state restores arrival order.
    // Requires Qt 6.1+, gated by the find_package above.
    mTableView->horizontalHeader()->setSortIndicatorClearable(true);
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
    // Tooltip reflects `Find`'s smart toggle behaviour.
    ui->actionFind->setToolTip(tr("Find in logs. Press again to close."));
    connect(ui->actionFind, &QAction::triggered, this, &MainWindow::Find);

    connect(ui->actionAddFilter, &QAction::triggered, this, [this]() { AddFilter(QUuid::createUuid().toString()); });
    connect(ui->actionClearAllFilters, &QAction::triggered, this, &MainWindow::ClearAllFilters);
    ui->actionClearAllFilters->setDisabled(true);

    // Sort actions. `actionSortBy` is the split-button face;
    // it just opens the per-column dropdown (sort has no
    // generic editor). `actionClearSort` is the single slot
    // shared across the Sort menu, toolbar, status-bar
    // indicator, and header right-click.
    connect(ui->actionClearSort, &QAction::triggered, this, &MainWindow::ClearSort);
    ui->actionClearSort->setDisabled(true);
    // Rebuild per-column entries on every open.
    if (ui->menuSort != nullptr)
    {
        connect(ui->menuSort, &QMenu::aboutToShow, this, &MainWindow::RebuildSortMenu);
    }

    // Stream toolbar; hidden until a live-tail stream is opened. The
    // same actions are also in the Stream menu.
    mStreamToolbar = addToolBar(tr("Stream"));
    mStreamToolbar->setObjectName(QStringLiteral("streamToolbar"));
    mStreamToolbar->addAction(ui->actionPauseStream);
    mStreamToolbar->addAction(ui->actionFollowTail);
    mStreamToolbar->addAction(ui->actionStopStream);
    mStreamToolbar->setVisible(false);
    // Both are Qt defaults; pinned explicitly so `TestStreamToolbarIsMovable`
    // keeps them from regressing.
    mStreamToolbar->setMovable(true);
    mStreamToolbar->setAllowedAreas(Qt::AllToolBarAreas);

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

    // Mirror Follow-newest into the view's pill-suppression flag.
    // Without this, Qt's signal ordering during a row insert can
    // briefly drop `mAtTailEdge` between the geometry pass and the
    // follow-up scroll-back, flashing the pill in the most common
    // live-tail steady state. `toggled` (not `triggered`) so a
    // programmatic `setChecked` from the pill-click handler below
    // takes the same path as a user toolbar click.
    connect(ui->actionFollowTail, &QAction::toggled, this, [this](bool checked) {
        if (mTableView != nullptr)
        {
            mTableView->SetPendingNewRowsSuppressed(checked);
        }
    });
    // Seed from the action's initial checked state: the `.ui`
    // declares Follow on by default but `toggled` only fires on
    // changes, so without this one-shot the view would start
    // un-suppressed.
    if (mTableView != nullptr)
    {
        mTableView->SetPendingNewRowsSuppressed(ui->actionFollowTail->isChecked());
    }

    // Pill click: acknowledge, scroll, and re-engage Follow newest
    // in live-tail. Keeping the policy here means the view stays
    // ignorant of proxies and the Follow action.
    connect(mTableView, &LogTableView::jumpToTailRequested, this, [this]() {
        // Acknowledge up-front so the count clears even when the
        // scroll below lands short of the visual tail (custom
        // sort placing source-newest in the middle of the proxy,
        // or the bounded-walk fallback snapping to the proxy tail
        // without crossing `maximum`).
        if (mTableView != nullptr)
        {
            mTableView->AcknowledgePendingNewRows();
        }
        JumpToNewestRow();
        // The pill click is an explicit "catch me up" command, so
        // unconditionally re-engage Follow in live tail (a manual
        // scroll-away would otherwise have disengaged it).
        if (IsLiveTailSession() && !ui->actionFollowTail->isChecked())
        {
            ui->actionFollowTail->setChecked(true);
        }
    });

    // Find bar lives in a dockable host so the user can float / dock
    // it and the layout round-trips through `saveState`. The hosted
    // `FindRecordWidget` keeps the same slots, so existing wiring
    // (see `FindRecords` below) is unchanged.
    mFindDock = new FindDock(this);
    addDockWidget(Qt::BottomDockWidgetArea, mFindDock);
    mFindDock->hide();
    mFindRecord = mFindDock->Widget();
    connect(mFindRecord, &FindRecordWidget::FindRecords, this, &MainWindow::FindRecords);
    connect(mFindRecord, &FindRecordWidget::MatchCountRequested, this, &MainWindow::UpdateFindMatchCount);

    // Every signal that can change the match set invalidates the
    // find bar's match cache. Hooked on the proxy only -- the
    // source-side signals chain through and would otherwise
    // double-fire. Column changes invalidate too because
    // `MatchRow` honours `Column::visible`.
    connect(mSortFilterProxyModel, &QAbstractItemModel::rowsInserted, this, &MainWindow::OnFindCacheInvalidated);
    connect(mSortFilterProxyModel, &QAbstractItemModel::rowsRemoved, this, &MainWindow::OnFindCacheInvalidated);
    connect(mSortFilterProxyModel, &QAbstractItemModel::layoutChanged, this, &MainWindow::OnFindCacheInvalidated);
    connect(mSortFilterProxyModel, &QAbstractItemModel::modelReset, this, &MainWindow::OnFindCacheInvalidated);
    connect(mSortFilterProxyModel, &QAbstractItemModel::columnsInserted, this, &MainWindow::OnFindCacheInvalidated);
    connect(mSortFilterProxyModel, &QAbstractItemModel::columnsRemoved, this, &MainWindow::OnFindCacheInvalidated);
    connect(mSortFilterProxyModel, &QAbstractItemModel::columnsMoved, this, &MainWindow::OnFindCacheInvalidated);
    // `dataChanged` covers in-place cell updates. Filter out style-only
    // emits via `IsStyleOnlyRoleChange` so theme repaints don't wake
    // the debounce timer for nothing.
    connect(
        mSortFilterProxyModel,
        &QAbstractItemModel::dataChanged,
        this,
        [this](const QModelIndex & /*topLeft*/, const QModelIndex & /*bottomRight*/, const QList<int> &roles) {
            if (LogModel::IsStyleOnlyRoleChange(roles))
            {
                return;
            }
            OnFindCacheInvalidated();
        }
    );

    // Status-bar rows-shown indicator. Subscribes to both source
    // and proxy because either can shift independently:
    //   - source `rowsInserted/Removed/modelReset`: streaming
    //     batches, session boundaries.
    //   - proxy `rowsInserted/Removed/modelReset/layoutChanged`:
    //     filter rule changes (which can move rows without
    //     changing the source row count). `layoutChanged` is
    //     proxy-only because it's how `LogFilterModel` reports
    //     "the predicate changed but the row pool didn't".
    // The slot itself is idempotent and cheap (two `rowCount()`
    // calls + a label assign), so duplicate fires from
    // adjacent signals are harmless.
    connect(mModel, &QAbstractItemModel::rowsInserted, this, &MainWindow::UpdateRowsShownStatus);
    connect(mModel, &QAbstractItemModel::rowsRemoved, this, &MainWindow::UpdateRowsShownStatus);
    connect(mModel, &QAbstractItemModel::modelReset, this, &MainWindow::UpdateRowsShownStatus);
    connect(mSortFilterProxyModel, &QAbstractItemModel::rowsInserted, this, &MainWindow::UpdateRowsShownStatus);
    connect(mSortFilterProxyModel, &QAbstractItemModel::rowsRemoved, this, &MainWindow::UpdateRowsShownStatus);
    connect(mSortFilterProxyModel, &QAbstractItemModel::modelReset, this, &MainWindow::UpdateRowsShownStatus);
    connect(mSortFilterProxyModel, &QAbstractItemModel::layoutChanged, this, &MainWindow::UpdateRowsShownStatus);

    // Sort indicator: keep `actionClearSort` and the status-bar
    // button in sync with the proxy's sort. `layoutChanged`
    // covers every sort permutation (header click, menu,
    // dropdown, programmatic clear, and the deferred-restore
    // path). Source row signals hide the indicator when the
    // model goes empty.
    connect(mSortFilterProxyModel, &QAbstractItemModel::layoutChanged, this, &MainWindow::UpdateSortStatus);
    connect(mSortFilterProxyModel, &QAbstractItemModel::modelReset, this, &MainWindow::UpdateSortStatus);
    connect(mModel, &QAbstractItemModel::rowsInserted, this, &MainWindow::UpdateSortStatus);
    connect(mModel, &QAbstractItemModel::rowsRemoved, this, &MainWindow::UpdateSortStatus);
    connect(mModel, &QAbstractItemModel::modelReset, this, &MainWindow::UpdateSortStatus);
    // Column rename emits `headerDataChanged` but no
    // `layoutChanged`, so without this hook the status-bar
    // tooltip would freeze on the old label. Only refresh when
    // the rename touches the sorted column - `UpdateSortStatus`
    // rebuilds all column labels and that's wasted work
    // otherwise.
    connect(
        mModel,
        &QAbstractItemModel::headerDataChanged,
        this,
        [this](Qt::Orientation orientation, int first, int last) {
            if (orientation != Qt::Horizontal || mSortFilterProxyModel == nullptr)
            {
                return;
            }
            const int sortColumn = mSortFilterProxyModel->SortColumn();
            if (sortColumn < 0 || sortColumn < first || sortColumn > last)
            {
                return;
            }
            UpdateSortStatus();
        }
    );

    mActionToggleFind = new QAction(tr("Find Bar"), this);
    mActionToggleFind->setObjectName(QStringLiteral("actionToggleFind"));
    mActionToggleFind->setCheckable(true);
    mActionToggleFind->setToolTip(tr("Show or hide the find bar. Ctrl+F focuses it; Ctrl+F again or Esc closes it."));
    addAction(mActionToggleFind);
    // Custom show callback: `RevealAndFocus` also moves keyboard focus
    // into the search field so the toggle behaves like every IDE find bar.
    WireDockToggle(
        mFindDock.data(),
        mActionToggleFind,
        &FindDock::closed,
        /*onShow=*/[this]() { mFindDock->RevealAndFocus(); }
    );
    // Catch up the match count after a reveal: the cache may have
    // been invalidated while the bar was hidden / buried, and the
    // current selection may have moved (so `i` can be stale even
    // when the row list is still correct). The cache-hit recount is
    // cheap; `BumpMatchCountDebounce` no-ops on an empty needle.
    connect(mFindDock, &FindDock::revealed, this, [this]() {
        if (mFindRecord != nullptr)
        {
            mFindRecord->BumpMatchCountDebounce();
        }
    });

    // Parse-errors dock replaces the old `QMessageBox::warning`
    // popups. Hidden until the first error of a session.
    mParseErrorsDock = new ParseErrorsDock(this);
    addDockWidget(Qt::BottomDockWidgetArea, mParseErrorsDock);
    mParseErrorsDock->hide();

    // Tabify the two bottom docks by default so they share the same
    // horizontal strip; `restoreState` overrides on later launches.
    tabifyDockWidget(mFindDock, mParseErrorsDock);

    mActionToggleParseErrors = new QAction(tr("Parse Errors"), this);
    mActionToggleParseErrors->setObjectName(QStringLiteral("actionToggleParseErrors"));
    mActionToggleParseErrors->setCheckable(true);
    mActionToggleParseErrors->setToolTip(tr("Show or hide the Parse Errors panel."));
    addAction(mActionToggleParseErrors);
    WireDockToggle(mParseErrorsDock.data(), mActionToggleParseErrors, &ParseErrorsDock::closed);
    connect(mParseErrorsDock, &ParseErrorsDock::countChanged, this, &MainWindow::UpdateParseErrorsStatus);
    // Auto-raise on the first batch of a session, unless the find
    // bar holds focus -- raising would yank focus mid-search. The
    // status-bar indicator is enough notice in that case.
    connect(mParseErrorsDock, &ParseErrorsDock::firstBatchArrived, this, [this]() {
        if (mParseErrorsDock == nullptr)
        {
            return;
        }
        if (FindBarHoldsFocus())
        {
            return;
        }
        if (!mParseErrorsDock->isVisible())
        {
            mParseErrorsDock->show();
        }
        mParseErrorsDock->raise();
    });

    // Record-detail dock: hidden by default; the View menu's Ctrl+I
    // toggle and row double-click both surface it.
    mRecordDetailDock = new RecordDetailDock(mModel, this);
    addDockWidget(Qt::RightDockWidgetArea, mRecordDetailDock);
    mRecordDetailDock->hide();

    // Tabify the two right-side docks by default; `restoreState`
    // (later in the constructor) overrides if the user moved them.
    tabifyDockWidget(mAnchorsDock, mRecordDetailDock);
    // `actionToggleRecordDetails` is declared in `main_window.ui` but
    // not placed in any `<addaction>`, so uic doesn't add it to any
    // widget's `actions()`. A QAction's shortcut only fires once it
    // is associated with a widget; add it here so `Ctrl+I` is live
    // from a cold launch, before the View menu is ever opened.
    addAction(ui->actionToggleRecordDetails);
    // On every visibility-true edge (reveal AND tab activation)
    // re-pull the table selection so the dock body reflects the
    // currently-focused row.
    WireDockToggle(
        mRecordDetailDock,
        ui->actionToggleRecordDetails,
        &RecordDetailDock::closed,
        /*onShow=*/{},
        /*onShown=*/[this]() { UpdateRecordDetailsFromSelection(); }
    );
    connect(mRecordDetailDock, &RecordDetailDock::openInNewWindowRequested, this, &MainWindow::OpenRecordDetailWindow);

    connect(mTableView, &QAbstractItemView::doubleClicked, this, &MainWindow::ShowRecordDetailsForProxyIndex);

    // Track selection changes through the live selection model;
    // centralised so a future `setModel` call only has to re-invoke
    // this helper.
    RebindRecordDetailSelectionTracking();

    // The dock owns its own `modelReset -> Clear` wiring, so reuse
    // outside `MainWindow` stays correct.

    mPreferencesEditor = new PreferencesEditor(mTheme, this);
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
    // Level-icons toggle: `SetShowLevelIcons` already emits a
    // scoped `dataChanged`; `ApplyLevelCellDelegate` then
    // attaches/detaches the delegate on the right column.
    connect(mPreferencesEditor, &PreferencesEditor::showLevelIconsChanged, this, [this](bool on) {
        if (mModel == nullptr)
        {
            return;
        }
        mModel->SetShowLevelIcons(on);
        ApplyLevelCellDelegate();
    });

    // High-contrast toggle: `SetHighContrast` rebuilds the style
    // cache and emits `themeChanged()`, reusing the normal
    // theme-swap repaint chain.
    connect(mPreferencesEditor, &PreferencesEditor::highContrastLevelsChanged, this, [this](bool on) {
        if (mTheme == nullptr)
        {
            return;
        }
        mTheme->SetHighContrast(on);
    });

    // Anchor hotkeys (programmatic so the .ui isn't bloated):
    //   Ctrl+1..8     anchor selection at colour N
    //   Ctrl+0        clear anchor on selection
    //   Ctrl+Shift+A  clear every anchor
    //   F2 / Shift+F2 jump to next / previous visible anchor
    for (std::size_t i = 0; i < mAnchorColorActions.size(); ++i)
    {
        auto *action = new QAction(this);
        action->setText(tr("Anchor selection in colour %1").arg(i + 1));
        action->setShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + static_cast<int>(i))));
        addAction(action);
        const int colourIndex = static_cast<int>(i);
        connect(action, &QAction::triggered, mTableView, [view = mTableView, colourIndex]() {
            view->AnchorSelection(colourIndex);
        });
        mAnchorColorActions[i] = action;
    }
    mActionClearRowAnchor = new QAction(tr("Remove anchor from selection"), this);
    mActionClearRowAnchor->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    addAction(mActionClearRowAnchor);
    connect(mActionClearRowAnchor, &QAction::triggered, mTableView, &LogTableView::ClearAnchorOnSelection);

    mActionJumpNextAnchor = new QAction(tr("Jump to next anchor"), this);
    mActionJumpNextAnchor->setShortcut(QKeySequence(Qt::Key_F2));
    addAction(mActionJumpNextAnchor);
    connect(mActionJumpNextAnchor, &QAction::triggered, this, [this]() { JumpToAnchor(true); });

    mActionJumpPrevAnchor = new QAction(tr("Jump to previous anchor"), this);
    mActionJumpPrevAnchor->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F2));
    addAction(mActionJumpPrevAnchor);
    connect(mActionJumpPrevAnchor, &QAction::triggered, this, [this]() { JumpToAnchor(false); });

    mActionClearAllAnchors = new QAction(tr("Clear all anchors"), this);
    mActionClearAllAnchors->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_A));
    addAction(mActionClearAllAnchors);
    connect(mActionClearAllAnchors, &QAction::triggered, this, [this]() {
        if (mAnchors != nullptr)
        {
            mAnchors->ClearAll();
        }
    });

    // Ctrl+/ opens the shortcuts reference. Registered programmatically so it
    // works without taking a slot in any menu.
    mActionShowShortcuts = new QAction(tr("Keyboard Shortcuts"), this);
    mActionShowShortcuts->setObjectName(QStringLiteral("actionShowShortcuts"));
    mActionShowShortcuts->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Slash));
    mActionShowShortcuts->setToolTip(tr("Show every keyboard shortcut available in this window."));
    addAction(mActionShowShortcuts);
    connect(mActionShowShortcuts, &QAction::triggered, this, &MainWindow::ShowShortcutsDialog);

    mStatusLabel = new QLabel(this);
    statusBar()->addPermanentWidget(mStatusLabel);
    mStatusLabel->hide();

    // Rows-shown indicator + inline Clear-filters button. Placed
    // immediately after `mStatusLabel` so the status reads
    // left-to-right as "Parsing foo - 12,345 lines | 8,432 of
    // 12,345 shown [Clear filters] [diagnostics] [parse errors]".
    // Both widgets are hidden by `UpdateRowsShownStatus` when the
    // source model is empty (e.g. before the first batch lands or
    // after `LogModel::Reset`).
    mRowsShownLabel = new QLabel(this);
    mRowsShownLabel->setObjectName(QStringLiteral("rowsShownLabel"));
    mRowsShownLabel->hide();
    statusBar()->addPermanentWidget(mRowsShownLabel);

    mClearFiltersStatusButton = new QPushButton(this);
    mClearFiltersStatusButton->setObjectName(QStringLiteral("clearFiltersStatusButton"));
    mClearFiltersStatusButton->setIcon(QApplication::style()->standardIcon(QStyle::SP_LineEditClearButton));
    mClearFiltersStatusButton->setText(tr("Clear filters"));
    mClearFiltersStatusButton->setToolTip(tr("Clear all active filters and show every row."));
    mClearFiltersStatusButton->setAccessibleName(tr("Clear all filters"));
    mClearFiltersStatusButton->setFlat(true);
    mClearFiltersStatusButton->setCursor(Qt::PointingHandCursor);
    mClearFiltersStatusButton->hide();
    statusBar()->addPermanentWidget(mClearFiltersStatusButton);
    // Route through the existing action so its enable/disable
    // logic stays the single source of truth and the menu, the
    // toolbar (when it lands), and this button all stay in lock-
    // step.
    connect(mClearFiltersStatusButton, &QPushButton::clicked, ui->actionClearAllFilters, &QAction::trigger);

    // Status-bar Clear-sort indicator. Mirrors the
    // Clear-filters button: hidden by default, surfaced by
    // `UpdateSortStatus` while a sort is active, click-routes
    // through `actionClearSort`.
    mClearSortStatusButton = new QPushButton(this);
    mClearSortStatusButton->setObjectName(QStringLiteral("clearSortStatusButton"));
    mClearSortStatusButton->setIcon(QApplication::style()->standardIcon(QStyle::SP_LineEditClearButton));
    mClearSortStatusButton->setText(tr("Clear sort"));
    mClearSortStatusButton->setAccessibleName(tr("Clear column sort"));
    // Fallback tooltip until `UpdateSortStatus` writes the
    // column-aware variant.
    mClearSortStatusButton->setToolTip(tr("Clear the active column sort."));
    mClearSortStatusButton->setFlat(true);
    mClearSortStatusButton->setCursor(Qt::PointingHandCursor);
    mClearSortStatusButton->hide();
    statusBar()->addPermanentWidget(mClearSortStatusButton);
    connect(mClearSortStatusButton, &QPushButton::clicked, ui->actionClearSort, &QAction::trigger);

    // 1 Hz tick that refreshes the live-tail elapsed time and the title's
    // running line count, so neither has to be rewritten per batch.
    constexpr int LIVE_TAIL_TICK_INTERVAL_MS = 1000;
    mLiveTailTickTimer = new QTimer(this);
    mLiveTailTickTimer->setInterval(LIVE_TAIL_TICK_INTERVAL_MS);
    connect(mLiveTailTickTimer, &QTimer::timeout, this, [this]() {
        UpdateStreamingStatus();
        UpdateWindowTitle();
    });

    mDiagnosticsButton = new QPushButton(this);
    mDiagnosticsButton->setObjectName(QStringLiteral("diagnosticsButton"));
    mDiagnosticsButton->setIcon(QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning));
    mDiagnosticsButton->setFlat(true);
    mDiagnosticsButton->setCursor(Qt::PointingHandCursor);
    mDiagnosticsButton->hide();
    statusBar()->addPermanentWidget(mDiagnosticsButton);
    connect(mDiagnosticsButton, &QPushButton::clicked, this, &MainWindow::ShowConfigurationDiagnostics);
    connect(mModel, &LogModel::columnHealthChanged, this, &MainWindow::UpdateDiagnosticsStatus);

    // Status-bar indicator for the parse-errors dock. Same UX as
    // `mDiagnosticsButton`: hides when empty, opens the dock on click.
    mParseErrorsStatusButton = new QPushButton(this);
    mParseErrorsStatusButton->setObjectName(QStringLiteral("parseErrorsStatusButton"));
    // Warning (not Critical): these are recoverable line-level
    // failures, not application-level fatals.
    mParseErrorsStatusButton->setIcon(QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning));
    mParseErrorsStatusButton->setFlat(true);
    mParseErrorsStatusButton->setCursor(Qt::PointingHandCursor);
    mParseErrorsStatusButton->hide();
    statusBar()->addPermanentWidget(mParseErrorsStatusButton);
    connect(mParseErrorsStatusButton, &QPushButton::clicked, this, [this]() {
        if (mParseErrorsDock == nullptr)
        {
            return;
        }
        if (!mParseErrorsDock->isVisible())
        {
            mParseErrorsDock->show();
        }
        mParseErrorsDock->raise();
    });

    connect(mModel, &LogModel::lineCountChanged, this, [this](qsizetype count) {
        mStreamingLineCount = count;
        UpdateStreamingStatus();
        // One-shot column auto-resize on the first non-empty batch.
        if (IsLiveTailSession() && !mFirstStreamingBatchSeen && count > 0)
        {
            mFirstStreamingBatchSeen = true;
            UpdateUi();
            // Reflect the just-pinned source name once; subsequent batches
            // are picked up by the 1 Hz live-tail tick.
            UpdateWindowTitle();
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
                    // Re-entrancy guard: the rewrite + downstream
                    // sync calls walk `mFilters`, so a transitive
                    // re-emit of `enumColumnsChanged` for the same
                    // column would see half-rewritten state.
                    if (mApplyingEnumRebuild)
                    {
                        return;
                    }
                    mApplyingEnumRebuild = true;
                    const auto demoteGuard = qScopeGuard([this]() { mApplyingEnumRebuild = false; });
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
                    // Resync the tooltip cache: it was built from
                    // the canonical names (`"Info"`, ...) and now
                    // needs the rewritten raw bytes.
                    SyncColumnFilterIndicators();
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

    // Seed "Show level icons" pref (default true). No explicit
    // `ApplyLevelCellDelegate` follow-up needed: the model has no
    // columns at ctor time, so the attach happens later via the
    // `modelReset`/`columnsInserted` connections above.
    if (mModel != nullptr)
    {
        const QSettings settings;
        const bool showLevelIcons = settings.value(QStringLiteral("ui/showLevelIcons"), true).toBool();
        mModel->SetShowLevelIcons(showLevelIcons);
    }

    // Run after every action is wired so they can all be decorated in one pass.
    FinaliseActionMetadata();

    // Persistent primary toolbar. Built after every referenced
    // action exists -- both .ui actions (`ui->action*`) and the
    // programmatic dock toggles (`mActionToggleFind`,
    // `mActionToggleAnchors`) and `mStreamToolbar` (the new bar
    // is inserted ahead of it) -- and before `RestoreWindowChrome`
    // so the persisted state can place the toolbar in its saved
    // dock area.
    BuildMainToolbar();

    // Run after every dock/toolbar has its `objectName` so `restoreState`
    // can resolve them. No-op on first launch.
    RestoreWindowChrome();

    // Settle the sort indicator's initial state. Earlier signal
    // hooks fired before the status-bar button existed, so sync
    // once now that both ends are wired.
    UpdateSortStatus();

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

namespace
{
/// True iff the payload carries at least one local file URL.
/// Refusing remote URLs flips the cursor to the no-drop indicator,
/// matching Explorer/Finder UX.
bool MimeHasLocalFileUrl(const QMimeData *mime)
{
    if (mime == nullptr || !mime->hasUrls())
    {
        return false;
    }
    const QList<QUrl> urls = mime->urls();
    if (urls.isEmpty())
    {
        return false;
    }
    return urls.first().isLocalFile();
}
} // namespace

void MainWindow::dragEnterEvent(QDragEnterEvent *event)
{
    if (MimeHasLocalFileUrl(event->mimeData()))
    {
        event->acceptProposedAction();
    }
}

void MainWindow::dragMoveEvent(QDragMoveEvent *event)
{
    if (MimeHasLocalFileUrl(event->mimeData()))
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
        // Skip during our own apply -- `OnThemeChanged` handles
        // the QSS re-apply once at the end. No-theme test path
        // also skips (nothing to re-evaluate).
        if (mTheme == nullptr || mTheme->IsApplyingTheme())
        {
            break;
        }
        // OS theme flip: re-evaluate Auto. If the resolved theme
        // is unchanged (Force mode, or same kind on Auto),
        // `OnThemeChanged` won't fire -- refresh the QSS manually
        // so palette-derived colours follow.
        const QString priorName = QString::fromStdString(mTheme->Active().name);
        mTheme->Reevaluate();
        const QString currentName = QString::fromStdString(mTheme->Active().name);
        if (priorName == currentName)
        {
            ApplyTableStyleSheet();
        }
        break;
    }
    case QEvent::StyleChange:
        if (mTheme != nullptr && mTheme->IsApplyingTheme())
        {
            break;
        }
        // External `qApp->setStyle` (defensive -- we have none).
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
    auto *child = new MainWindow(mTheme, mHistoryManager, nullptr);
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
    // Defer the loaded sort until streaming finishes (see
    // `ApplyDeferredSortFromConfig` for the O(N^2) avoidance).
    mPendingApplySortFromConfig = true;
    if (!DoLoadConfiguration(jsonPath))
    {
        mPendingApplySortFromConfig = false;
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

    // Defer the loaded sort until streaming finishes (see
    // `ApplyDeferredSortFromConfig`). Set *after* `NewSession`,
    // which clears the latch.
    mPendingApplySortFromConfig = true;

    // Apply failure surfaces a `QMessageBox`; bail without queueing
    // files so the view is at least empty rather than mixed.
    if (!ApplyLoadedConfiguration(std::move(parsed)))
    {
        mPendingApplySortFromConfig = false;
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
        // No source -- columns / filters are installed but there's
        // nothing to stream. Consume the deferred-sort latch so it
        // doesn't leak across the next session restore.
        ApplyDeferredSortFromConfig();
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
        // Non-File: no streaming either; consume the deferral so
        // the latch can't outlive this attempt.
        ApplyDeferredSortFromConfig();
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

    // Anchors are session-scoped. Clear after the model reset so
    // the resulting refresh runs against the empty row set.
    if (mAnchors != nullptr)
    {
        mAnchors->ClearAll();
    }

    // Parse errors are session-scoped. `ResetSessionState` re-arms
    // the auto-raise for the new session. The per-file watermark
    // mirrors the dock reset so the next session's first batch
    // starts at index 0.
    if (mParseErrorsDock != nullptr)
    {
        mParseErrorsDock->ResetSessionState();
    }
    mStreamingErrorsCut = 0;

    mCurrentSource.reset();
    mSessionMode = SessionMode::Idle;
    mLastTerminalSessionMode = SessionMode::Idle;
    mStreamingFileName.clear();
    mStreamingLineCount = 0;
    mStreamingErrorCount = 0;
    mFirstStreamingBatchSeen = false;
    mSourceWaiting = false;
    // The configuration that requested the deferred sort is gone.
    mPendingApplySortFromConfig = false;
    // Drop the pinned uuid + open-windows membership so the next
    // AutoSave creates a fresh entry and a crash before then
    // doesn't re-restore the discarded session.
    DetachAutoSaveUuid();
    SetConfigurationUiEnabled(true);
    UpdateStreamToolbarVisibility();
    UpdateStreamingStatus();
    UpdateWindowTitle();
    UpdateUi();
}

void MainWindow::OpenFiles()
{
    // Sample modifier state before the modal: `keyboardModifiers()`
    // after the dialog reports whatever is held *now*, almost never
    // what the user held on menu activation.
    const bool forceReplace = QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);

    const QStringList files = QFileDialog::getOpenFileNames(
        this,
        tr("Select Log Files"),
        DefaultOpenDir(),
        tr("Structured Logs (*.json *.jsonl *.ndjson *.logfmt);;All Files (*.*)")
    );
    if (files.isEmpty())
    {
        return;
    }
    RememberLastOpenDir(files.first());

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

        // Bulk-replace anchors before RebuildFiltersFromConfiguration
        // mirrors the now-empty `AnchorManager` back onto disk. Any
        // dropped (future-schema) entries are surfaced to the user.
        if (mAnchors != nullptr)
        {
            const std::size_t droppedAnchorCount = mAnchors->Replace(mModel->Configuration().anchors);
            if (droppedAnchorCount > 0)
            {
                statusBar()->showMessage(
                    tr("%1 anchor(s) from a newer schema were dropped.")
                        .arg(static_cast<qulonglong>(droppedAnchorCount)),
                    STATUS_BAR_MESSAGE_TIMEOUT_MS
                );
            }
        }

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
    // Defer the sort until streaming finishes (see
    // `ApplyDeferredSortFromConfig`).
    mPendingApplySortFromConfig = true;
    if (!DoLoadConfiguration(configPath))
    {
        // TOCTOU: the file was rewritten between probe and commit.
        // The model was reset before the throw, so streaming logs
        // against the unintended default columns would mislead --
        // bail without queueing anything.
        mPendingApplySortFromConfig = false;
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
        // Anchors are session-scoped; preserved on append.
        if (mAnchors != nullptr)
        {
            mAnchors->ClearAll();
        }
        // Session-scoped; `ResetSessionState` re-arms the auto-raise.
        // Watermark resets in lockstep with the dock + model error vector.
        if (mParseErrorsDock != nullptr)
        {
            mParseErrorsDock->ResetSessionState();
        }
        mStreamingErrorsCut = 0;
        mCurrentSource.reset();
        mSessionMode = SessionMode::Idle;
        mLastTerminalSessionMode = SessionMode::Idle;
        // Destructive open: drop the previous session's deferred-sort intent.
        mPendingApplySortFromConfig = false;
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

    // Per-file batch under a header that names the file (or stream)
    // that just finished. Fires *before* the chaining check so the
    // intermediate file's errors don't get folded into the last
    // file's batch in a multi-file static open. `mStreamingFileName`
    // is still set to the just-finished source here -- the chaining
    // path below will overwrite it for the next file.
    //
    // Open-failure entries in `mPendingOpenErrors` are intentionally
    // *not* folded in here: they aren't tied to any one streamed file
    // and would be misleading under a file-named header. They're
    // surfaced under their own batch at the bottom of this function.
    if (result == StreamingResult::Success)
    {
        const auto &allErrors = mModel->StreamingErrors();
        // `std::min` guards against a model reset that landed between
        // our last slice and now (would leave the watermark past end).
        const size_t cut = std::min(mStreamingErrorsCut, allErrors.size());
        if (cut < allErrors.size())
        {
            const std::vector<std::string> thisFileErrors(
                allErrors.begin() + static_cast<std::ptrdiff_t>(cut), allErrors.end()
            );
            const QString title = mStreamingFileName.isEmpty()
                                      ? tr("Error Parsing Logs")
                                      : tr("Error Parsing Logs \u2014 %1").arg(mStreamingFileName);
            ShowParseErrors(title, thisFileErrors);
        }
        mStreamingErrorsCut = allErrors.size();
    }

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

    // Stop the 1 Hz refresh; the elapsed value is kept so the final
    // status line still names the session length.
    StopLiveTailTicker();

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
    // Rebuild the title's "(<n> lines)" suffix now that streaming is over
    // and the tick timer that was driving it has stopped.
    UpdateWindowTitle();
    // Refresh the column-health snapshot now that parsing has
    // settled. Drives the header warning glyph and the status-bar
    // mismatch summary via `columnHealthChanged`.
    mModel->RefreshColumnHealth();
    // Per-file parse-error batches were already surfaced at the top
    // of this function (one per file in the chain). What remains is
    // any open-failure residue from the multi-file queue -- those
    // entries are tied to files that never streamed, so they get
    // their own dedicated batch instead of being mislabeled under
    // a streamed-file header.
    if (result == StreamingResult::Success && !mPendingOpenErrors.empty())
    {
        ShowParseErrors(tr("Error Opening File"), mPendingOpenErrors);
    }
    mPendingOpenErrors.clear();
    mStreamingFileName.clear();
    // Keep `mCurrentSource` on Success / Cancelled (rows are
    // still present, descriptor still describes them); drop it
    // on Failed where there is nothing left to describe.
    if (result == StreamingResult::Failed)
    {
        mCurrentSource.reset();
    }

    // Apply the deferred sort before the auto-save below so the
    // mirror reads the applied sort from the proxy. Runs on every
    // terminal result; no-op when the latch is clear or the user
    // sorted mid-stream.
    ApplyDeferredSortFromConfig();

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
                .format = DetectFormatForPath(std::filesystem::path(file.toStdString())),
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
        UpdateWindowTitle();

        auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(logFile));
        loglib::FileLineSource *fileSourcePtr = fileSource.get();
        QtStreamingLogSink *sink = mModel->Sink();

        loglib::ParserOptions options;
        options.configuration = std::move(cfg);

        // Reads `mCurrentSource->format` once on the GUI thread; the
        // worker captures the resulting parser by value so a later
        // session switch on the GUI cannot retarget the in-flight
        // parse. Defaults to Json when no source descriptor is
        // present yet (first open of a fresh session).
        const loglib::LogConfiguration::Source::Format format =
            mCurrentSource ? mCurrentSource->format : loglib::LogConfiguration::Source::Format::Json;
        std::shared_ptr<loglib::LogParser> parser = MakeParserForFormat(format);

        // False positive: `parseCallable` is moved into the model and invoked;
        // `cfg` is consumed by `options`.
        // NOLINTNEXTLINE(clang-analyzer-cplusplus.NewDeleteLeaks)
        auto parseCallable = [sink, fileSourcePtr, options = std::move(options), parser = std::move(parser)](
                                 const loglib::StopToken &stopToken
                             ) mutable {
            options.stopToken = stopToken;
            parser->ParseStreaming(*fileSourcePtr, *sink, options);
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
        ShowParseErrors(tr("Error Opening File"), mPendingOpenErrors);
        mPendingOpenErrors.clear();
    }
}

void MainWindow::OpenLogStream()
{
    const QString file = QFileDialog::getOpenFileName(
        this,
        tr("Open Log Stream..."),
        DefaultOpenDir(),
        tr("Structured Logs (*.json *.jsonl *.ndjson *.logfmt);;All Files (*.*)")
    );
    if (file.isEmpty())
    {
        return;
    }
    RememberLastOpenDir(file);
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
            tr("Error Opening Log Stream"),
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
    // Anchors are session-scoped.
    if (mAnchors != nullptr)
    {
        mAnchors->ClearAll();
    }
    // Session-scoped; `ResetSessionState` re-arms the auto-raise.
    // Watermark resets in lockstep with the dock + model error vector.
    if (mParseErrorsDock != nullptr)
    {
        mParseErrorsDock->ResetSessionState();
    }
    mStreamingErrorsCut = 0;
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
            .format = DetectFormatForPath(std::filesystem::path(file.toStdString())),
            .locators = {displayPath},
            .locatorDedupKeys = {dedupKey}
        };
    }
    mSessionMode = SessionMode::LiveTail;
    mStreamingLineCount = 0;
    mStreamingErrorCount = 0;
    mFirstStreamingBatchSeen = false;
    SetConfigurationUiEnabled(false);
    StartLiveTailTicker();
    UpdateStreamingStatus();
    UpdateStreamToolbarVisibility();
    UpdateWindowTitle();
    ApplyDisplayOrder();

    auto cfg = std::make_shared<const loglib::LogConfiguration>(mModel->Configuration());

    loglib::ParserOptions options;
    options.configuration = std::move(cfg);

    // Wrap the producer in a `StreamLineSource` so each `LogLine` can
    // resolve its bytes via `LineSource::RawLine` later.
    auto streamSource = std::make_unique<loglib::StreamLineSource>(filePath, std::move(source));
    const loglib::LogConfiguration::Source::Format format =
        mCurrentSource ? mCurrentSource->format : loglib::LogConfiguration::Source::Format::Json;
    auto parserFactory = [format]() { return MakeParserForFormat(format); };
    mModel->BeginStreaming(std::move(streamSource), std::move(options), std::move(parserFactory));
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
            tr("Error Opening Network Stream"),
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
    // Anchors are session-scoped.
    if (mAnchors != nullptr)
    {
        mAnchors->ClearAll();
    }
    // Session-scoped; `ResetSessionState` re-arms the auto-raise.
    // Watermark resets in lockstep with the dock + model error vector.
    if (mParseErrorsDock != nullptr)
    {
        mParseErrorsDock->ResetSessionState();
    }
    mStreamingErrorsCut = 0;
    DetachAutoSaveUuid();

    mStreamingFileName = QString::fromStdString(displayName);
    // Network-stream locator is a producer URI, not a filesystem
    // path -- no canonicalisation applies, so dedup key == display.
    // Both arrays populated so the parallel-array invariant holds.
    const loglib::LogConfiguration::Source::Format dialogFormat = (cfg.format == NetworkStreamDialog::Format::Logfmt)
                                                                      ? loglib::LogConfiguration::Source::Format::Logfmt
                                                                      : loglib::LogConfiguration::Source::Format::Json;
    mCurrentSource = loglib::LogConfiguration::Source{
        .kind = loglib::LogConfiguration::Source::Kind::NetworkStream,
        .format = dialogFormat,
        .locators = {displayName},
        .locatorDedupKeys = {displayName}
    };
    mSessionMode = SessionMode::LiveTail;
    mStreamingLineCount = 0;
    mStreamingErrorCount = 0;
    mFirstStreamingBatchSeen = false;
    SetConfigurationUiEnabled(false);
    StartLiveTailTicker();
    UpdateStreamingStatus();
    UpdateStreamToolbarVisibility();
    UpdateWindowTitle();
    ApplyDisplayOrder();

    auto config = std::make_shared<const loglib::LogConfiguration>(mModel->Configuration());
    loglib::ParserOptions options;
    options.configuration = std::move(config);

    // Network streams have no real filesystem path; the producer's
    // display string serves as the LineSource's opaque identity.
    auto streamSource =
        std::make_unique<loglib::StreamLineSource>(std::filesystem::path(displayName), std::move(producer));
    const loglib::LogConfiguration::Source::Format format =
        mCurrentSource ? mCurrentSource->format : loglib::LogConfiguration::Source::Format::Json;
    auto parserFactory = [format]() { return MakeParserForFormat(format); };
    mModel->BeginStreaming(std::move(streamSource), std::move(options), std::move(parserFactory));
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

void MainWindow::ShowShortcutsDialog()
{
    if (mShortcutsDialog.isNull())
    {
        mShortcutsDialog = new ShortcutsDialog(this, this);
        mShortcutsDialog->setAttribute(Qt::WA_DeleteOnClose, false);
    }
    mShortcutsDialog->show();
    mShortcutsDialog->raise();
    mShortcutsDialog->activateWindow();
}

namespace
{
constexpr auto SETTINGS_GEOMETRY_KEY = "ui/mainWindow/geometry";
constexpr auto SETTINGS_STATE_KEY = "ui/mainWindow/state";

/// Display name for the active source, or empty when idle.
/// File sources become a basename so they fit a typical title bar;
/// network streams keep their producer-supplied label.
QString CurrentSourceLabel(const std::optional<loglib::LogConfiguration::Source> &source, const QString &streamingName)
{
    if (!source.has_value() || source->locators.empty())
    {
        // Streaming has named the file but the source isn't pinned yet.
        return streamingName;
    }
    // Non-const so the trailing `return first` can move; see
    // clang-tidy `performance-no-automatic-move`.
    QString first = QString::fromStdString(source->locators.front());
    if (source->kind == loglib::LogConfiguration::Source::Kind::File)
    {
        QString basename = QFileInfo(first).fileName();
        if (!basename.isEmpty())
        {
            return basename;
        }
    }
    return first;
}
} // namespace

void MainWindow::SaveWindowChrome() const
{
    QSettings settings;
    settings.setValue(QString::fromLatin1(SETTINGS_GEOMETRY_KEY), saveGeometry());
    settings.setValue(QString::fromLatin1(SETTINGS_STATE_KEY), saveState());
}

void MainWindow::RestoreWindowChrome()
{
    const QSettings settings;
    const QByteArray geometry = settings.value(QString::fromLatin1(SETTINGS_GEOMETRY_KEY)).toByteArray();
    const QByteArray state = settings.value(QString::fromLatin1(SETTINGS_STATE_KEY)).toByteArray();
    // Both calls are no-ops on empty input, so first launch falls through
    // to Qt's default geometry.
    if (!geometry.isEmpty())
    {
        restoreGeometry(geometry);
    }
    if (!state.isEmpty())
    {
        restoreState(state);
    }
}

void MainWindow::UpdateWindowTitle()
{
    const QString appName = tr("Structured Log Viewer");
    const QString sourceLabel = CurrentSourceLabel(mCurrentSource, mStreamingFileName);

    QString title;
    if (sourceLabel.isEmpty())
    {
        title = appName;
    }
    else
    {
        // Build the "<count> lines" suffix, falling back to the model's row
        // count once streaming has reset `mStreamingLineCount` (which only
        // happens on `NewSession` / discard paths, never mid-stream).
        qsizetype lines = mStreamingLineCount;
        if (lines == 0 && mModel != nullptr)
        {
            lines = mModel->rowCount();
        }
        const QString lineCount = QLocale::system().toString(static_cast<qlonglong>(lines));
        QString suffix;
        if (IsLiveTailSession())
        {
            // U+00B7 MIDDLE DOT between the badge and the count.
            suffix = tr("Live tail \u00B7 %1 lines").arg(lineCount);
        }
        else if (lines > 0)
        {
            suffix = tr("%1 lines").arg(lineCount);
        }
        // U+2014 EM DASH between the source and app names, matching the
        // macOS/GNOME proxy-title convention.
        if (suffix.isEmpty())
        {
            title = QStringLiteral("%1 \u2014 %2").arg(sourceLabel, appName);
        }
        else
        {
            title = tr("%1 \u2014 %2 (%3)").arg(sourceLabel, appName, suffix);
        }
    }

    // `[*]` is Qt's modified-marker placeholder; it's rendered iff
    // `isWindowModified()` is true. Always appended so the asterisk can
    // toggle without rebuilding the whole title.
    title += QStringLiteral("[*]");
    setWindowTitle(title);
    setWindowModified(mFiltersDirty);

    // Proxy-icon hint for OS title bars (macOS shows the file glyph;
    // recent Windows uses it for jumplist grouping). Only meaningful
    // for file sources; cleared otherwise.
    if (mCurrentSource.has_value() && mCurrentSource->kind == loglib::LogConfiguration::Source::Kind::File &&
        !mCurrentSource->locators.empty())
    {
        setWindowFilePath(QString::fromStdString(mCurrentSource->locators.front()));
    }
    else
    {
        setWindowFilePath(QString());
    }
}

void MainWindow::MarkFiltersDirty()
{
    if (mLoadingConfiguration)
    {
        return;
    }
    if (mFiltersDirty)
    {
        return;
    }
    mFiltersDirty = true;
    UpdateWindowTitle();
}

QString MainWindow::DefaultOpenDir() const
{
    const QSettings settings;
    // Non-const so the early return can move; see clang-tidy
    // `performance-no-automatic-move`.
    QString remembered = settings.value(QStringLiteral("ui/lastOpenDir")).toString();
    if (!remembered.isEmpty() && QFileInfo(remembered).isDir())
    {
        return remembered;
    }
    // Documents is the platform's idiomatic landing zone for ad-hoc opens,
    // matching Notepad / Console.app / VS Code defaults.
    return QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
}

void MainWindow::RememberLastOpenDir(const QString &path)
{
    if (path.isEmpty())
    {
        return;
    }
    const QString dir = QFileInfo(path).absolutePath();
    if (dir.isEmpty())
    {
        return;
    }
    QSettings settings;
    settings.setValue(QStringLiteral("ui/lastOpenDir"), dir);
}

void MainWindow::FinaliseActionMetadata()
{
    // Walk every action on the window. Skipping tooltips that already
    // mention the shortcut leaves .ui-defined "(Ctrl+X)" tooltips alone.
    const QList<QAction *> actions = findChildren<QAction *>();
    for (QAction *action : actions)
    {
        if (action == nullptr || action->isSeparator())
        {
            continue;
        }
        const QString shortcut = action->shortcut().toString(QKeySequence::NativeText);
        const bool hasShortcut = !shortcut.isEmpty();

        QString tooltip = action->toolTip();
        const QString text = action->text();
        if (hasShortcut && !tooltip.contains(shortcut, Qt::CaseInsensitive))
        {
            // No tooltip yet — derive one from the action text (sans `&` accelerators).
            if (tooltip.isEmpty() || tooltip == text)
            {
                tooltip = text;
                tooltip.replace(QStringLiteral("&&"), QStringLiteral("\x1F"));
                tooltip.remove(QLatin1Char('&'));
                tooltip.replace(QStringLiteral("\x1F"), QStringLiteral("&"));
            }
            tooltip = tooltip + QStringLiteral(" (") + shortcut + QStringLiteral(")");
            action->setToolTip(tooltip);
        }

        // Mirror the (possibly just-suffixed) tooltip into statusTip so
        // QMainWindow shows it on hover for free.
        if (action->statusTip().isEmpty() && !tooltip.isEmpty())
        {
            action->setStatusTip(tooltip);
        }
    }
}

namespace
{
/// Formats @p ms as `HH:MM:SS`, or `MM:SS` for sub-hour sessions.
QString FormatElapsed(qint64 ms)
{
    constexpr qint64 MS_PER_SEC = 1000;
    constexpr qint64 SEC_PER_MIN = 60;
    constexpr qint64 SEC_PER_HOUR = 60 * SEC_PER_MIN;
    constexpr int FIELD_WIDTH = 2;
    constexpr int DECIMAL_BASE = 10;

    const qint64 totalSec = ms / MS_PER_SEC;
    const qint64 hours = totalSec / SEC_PER_HOUR;
    const qint64 minutes = (totalSec % SEC_PER_HOUR) / SEC_PER_MIN;
    const qint64 seconds = totalSec % SEC_PER_MIN;
    if (hours > 0)
    {
        return QStringLiteral("%1:%2:%3")
            .arg(hours, FIELD_WIDTH, DECIMAL_BASE, QLatin1Char('0'))
            .arg(minutes, FIELD_WIDTH, DECIMAL_BASE, QLatin1Char('0'))
            .arg(seconds, FIELD_WIDTH, DECIMAL_BASE, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2")
        .arg(minutes, FIELD_WIDTH, DECIMAL_BASE, QLatin1Char('0'))
        .arg(seconds, FIELD_WIDTH, DECIMAL_BASE, QLatin1Char('0'));
}
} // namespace

void MainWindow::UpdateStreamingStatus()
{
    if (!IsSessionActive())
    {
        mStatusLabel->clear();
        mStatusLabel->hide();
        return;
    }

    // Locale-grouped digits so big counts read as "12,345 lines".
    const QLocale loc = QLocale::system();
    const QString lineCount = loc.toString(static_cast<qlonglong>(mStreamingLineCount));
    const QString errorCount = loc.toString(static_cast<qlonglong>(mStreamingErrorCount));

    QString text;
    if (!IsLiveTailSession())
    {
        text = tr("Parsing %1 - %2 lines, %3 errors").arg(mStreamingFileName, lineCount, errorCount);
    }
    else if (mSourceWaiting)
    {
        // Source unavailable takes precedence over Paused.
        text = tr("Source unavailable - last seen %1 - %2 lines, %3 errors")
                   .arg(mStreamingFileName, lineCount, errorCount);
    }
    else if (mModel->Sink() && mModel->Sink()->IsPaused())
    {
        const auto buffered = static_cast<qlonglong>(mModel->Sink()->PausedLineCount());
        text = tr("Paused - %1 lines, %2 buffered").arg(lineCount, loc.toString(buffered));
    }
    else
    {
        text = tr("Streaming %1 - %2 lines, %3 errors").arg(mStreamingFileName, lineCount, errorCount);
    }

    // Paused-drop telemetry stays non-zero across Resume so the user
    // keeps seeing "lines were lost" until Stop.
    if (IsLiveTailSession() && mModel->Sink())
    {
        const auto dropped = static_cast<qlonglong>(mModel->Sink()->PausedDropCount());
        if (dropped > 0)
        {
            text += tr(", %1 dropped while paused").arg(loc.toString(dropped));
        }
    }

    if (IsLiveTailSession() && mLiveTailTimer.isValid())
    {
        text += tr(" - %1 since start").arg(FormatElapsed(mLiveTailTimer.elapsed()));
    }

    if (IsLiveTailSession() && mRotationFlashActive)
    {
        text += tr(" - rotated");
    }

    mStatusLabel->setText(text);
    mStatusLabel->show();
}

void MainWindow::UpdateRowsShownStatus()
{
    if (mRowsShownLabel == nullptr || mClearFiltersStatusButton == nullptr)
    {
        return;
    }

    // Gate on "is there data to count?" rather than session state.
    // `OnStreamingFinished` flips `mSessionMode` back to `Idle` for
    // finite static loads, but the user keeps browsing the rows --
    // hiding the count there would surface only during streaming
    // and disappear the moment the parse completed.
    const int sourceRows = (mModel != nullptr) ? mModel->rowCount() : 0;
    const int proxyRows = (mSortFilterProxyModel != nullptr) ? mSortFilterProxyModel->rowCount() : 0;
    if (sourceRows <= 0)
    {
        mRowsShownLabel->clear();
        mRowsShownLabel->hide();
        mClearFiltersStatusButton->hide();
        return;
    }

    const QLocale loc = QLocale::system();
    QString text;
    if (proxyRows < sourceRows)
    {
        text =
            tr("%1 of %2 shown")
                .arg(loc.toString(static_cast<qlonglong>(proxyRows)), loc.toString(static_cast<qlonglong>(sourceRows)));
    }
    else if (sourceRows == 1)
    {
        text = tr("%1 line").arg(loc.toString(static_cast<qlonglong>(sourceRows)));
    }
    else
    {
        text = tr("%1 lines").arg(loc.toString(static_cast<qlonglong>(sourceRows)));
    }
    // Skip the `setText` (and the resulting repaint / re-layout of the
    // permanent status-bar area) when the digits haven't moved.
    // `rowsInserted` fires multiple times per streaming batch -- once
    // from the source and once from the proxy -- so this elides one
    // of every two paints under load.
    if (mRowsShownLabel->text() != text)
    {
        mRowsShownLabel->setText(text);
    }
    mRowsShownLabel->show();

    // Decoupled from `proxyRows < sourceRows`: a filter that matches
    // every row leaves the counts equal but `mFilters` populated,
    // and the user still wants the affordance to clear it.
    mClearFiltersStatusButton->setVisible(!mFilters.empty());
}

void MainWindow::StartLiveTailTicker()
{
    mLiveTailTimer.start();
    if (mLiveTailTickTimer != nullptr)
    {
        mLiveTailTickTimer->start();
    }
}

void MainWindow::StopLiveTailTicker()
{
    if (mLiveTailTickTimer != nullptr)
    {
        mLiveTailTickTimer->stop();
    }
    // Leave `mLiveTailTimer` armed so the final status line can still report
    // the session length. It's restarted on the next live-tail open.
}

void MainWindow::BuildMainToolbar()
{
    // Two adjacent toolbars on the same row: the new primary
    // toolbar hosts the persistent actions, and `mStreamToolbar`
    // continues to surface only during live-tail. `insertToolBar`
    // lands the new bar *before* the stream bar in the top dock
    // area, so the combined strip reads "Main | Stream"
    // left-to-right when both are visible.
    mMainToolbar = new QToolBar(tr("Main"), this);
    mMainToolbar->setObjectName(QStringLiteral("mainToolbar"));
    mMainToolbar->setMovable(true);
    mMainToolbar->setAllowedAreas(Qt::AllToolBarAreas);
    // Icon-only keeps the bar compact; `FinaliseActionMetadata`
    // has already populated each action's tooltip with the
    // shortcut, so hover-help still names what every button does.
    mMainToolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
    // 20px is `PM_LargeIconSize` on Windows / macOS; pinning the
    // edge length keeps the bar visually consistent even when a
    // theme swaps the active `QStyle` (which can shift the metric).
    constexpr int TOOLBAR_ICON_PX = 20;
    const QSize toolbarIconSize{TOOLBAR_ICON_PX, TOOLBAR_ICON_PX};
    mMainToolbar->setIconSize(toolbarIconSize);

    insertToolBar(mStreamToolbar, mMainToolbar);

    // Stream toolbar shares the row, so mirror the visual policy:
    // icon-only + matching icon edge length. The actions on the
    // stream toolbar get themed icons below; without this the
    // combined strip would jump from compact-icon (main) to
    // icon+text (stream) mid-row and look unfinished. Tooltips
    // (assigned in the .ui) still name each button on hover.
    if (mStreamToolbar != nullptr)
    {
        mStreamToolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
        mStreamToolbar->setIconSize(toolbarIconSize);
    }

    // Stash the SVG path on each action AND register it in
    // `mThemedActions` paired with the widget that will drive its
    // render policy (palette / iconSize / DPR). `RefreshThemedIcons`
    // walks the registry rather than `QToolBar::actions()`, so
    // actions reached through `addWidget` (the split button's
    // default action, its dropdown menu entries) participate in
    // the refresh -- without this they would be wrapped in an
    // internal `QWidgetAction` invisible to a toolbar-iteration
    // refresh and the split button would render blank.
    //
    // Actions without an `svgIconPath` property are skipped by the
    // refresh loop, so future actions that ship their own QIcon
    // don't accidentally get clobbered.
    //
    // `svgIconPathChecked` is a second optional property for
    // checkable actions whose On state needs a different glyph
    // (e.g. Pause -> Play when paused). When absent the refresh
    // loop reuses the Off pixmap for the On state, so most actions
    // need only the single tag.
    //
    // `mThemedActions.clear()` defends against any future caller
    // that ever runs `BuildMainToolbar` twice: without it the
    // registry would grow duplicate entries and `RefreshThemedIcons`
    // would do redundant work, plus stale-anchor entries (the
    // first build's toolbar is gone) would litter the list.
    mThemedActions.clear();
    const auto tag =
        [this](QAction *action, QWidget *anchor, const QString &resourcePath, const QString &checkedResourcePath = {}) {
            if (action == nullptr)
            {
                return;
            }
            action->setProperty("svgIconPath", resourcePath);
            if (!checkedResourcePath.isEmpty())
            {
                action->setProperty("svgIconPathChecked", checkedResourcePath);
            }
            mThemedActions.append({.action = QPointer<QAction>(action), .anchor = QPointer<QWidget>(anchor)});
        };

    tag(ui->actionOpen, mMainToolbar, QStringLiteral(":/icons/folder-open.svg"));
    // The open-stream actions live behind the split button (added
    // via `addWidget` below). Anchor them to `mMainToolbar` so
    // their pixmaps are rasterised at the toolbar's iconSize and
    // the split button's default-action sync picks up a non-empty
    // icon. `actionOpenNetworkStream` only appears in the popup
    // menu but is anchored to the toolbar too so its size matches
    // the rest of the strip and theme flips refresh it through
    // the same loop.
    tag(ui->actionOpenLogStream, mMainToolbar, QStringLiteral(":/icons/square-play.svg"));
    tag(ui->actionOpenNetworkStream, mMainToolbar, QStringLiteral(":/icons/radio-tower.svg"));
    tag(ui->actionAddFilter, mMainToolbar, QStringLiteral(":/icons/funnel-plus.svg"));
    tag(ui->actionClearAllFilters, mMainToolbar, QStringLiteral(":/icons/funnel-x.svg"));
    tag(ui->actionSortBy, mMainToolbar, QStringLiteral(":/icons/arrow-down-up.svg"));
    tag(ui->actionClearSort, mMainToolbar, QStringLiteral(":/icons/circle-x.svg"));
    tag(mActionToggleFind, mMainToolbar, QStringLiteral(":/icons/search.svg"));
    tag(ui->actionToggleRecordDetails, mMainToolbar, QStringLiteral(":/icons/panel-right-open.svg"));
    tag(mActionToggleAnchors, mMainToolbar, QStringLiteral(":/icons/bookmark.svg"));
    tag(ui->actionPreferences, mMainToolbar, QStringLiteral(":/icons/settings-2.svg"));
    // Stream toolbar gets the same treatment so the combined strip
    // looks uniform when both bars are visible. Pause is the one
    // action where the On state is semantically distinct from Off
    // (paused vs running), so we override its checked glyph with
    // the play icon -- users expect the button to invite the
    // opposite transition, mirroring media-player conventions.
    tag(ui->actionPauseStream,
        mStreamToolbar,
        QStringLiteral(":/icons/pause.svg"),
        QStringLiteral(":/icons/square-play.svg"));
    tag(ui->actionFollowTail, mStreamToolbar, QStringLiteral(":/icons/arrow-down-to-line.svg"));
    tag(ui->actionStopStream, mStreamToolbar, QStringLiteral(":/icons/square.svg"));

    mMainToolbar->addAction(ui->actionOpen);

    // Split button: primary click opens the log-file stream; the
    // dropdown surfaces the network variant. `MenuButtonPopup`
    // (not `InstantPopup`) keeps the more-common log path one
    // click away while making the network entry discoverable.
    // `setDefaultAction` would normally also wire the button's
    // icon -- so the explicit `setIcon` from `RefreshThemedIcons`
    // happens *after* the menu / default-action plumbing is in
    // place and re-installs the themed icon.
    auto *openStreamButton = new QToolButton(mMainToolbar);
    openStreamButton->setObjectName(QStringLiteral("openStreamSplitButton"));
    openStreamButton->setDefaultAction(ui->actionOpenLogStream);
    openStreamButton->setPopupMode(QToolButton::MenuButtonPopup);
    // `addWidget` keeps custom buttons out of the toolbar's
    // auto-layout, so the toolbar's iconSize / button-style do
    // NOT propagate. Mirror them explicitly so the split button
    // sits in the strip at the same edge length and icon-only
    // policy as every other action.
    openStreamButton->setIconSize(toolbarIconSize);
    openStreamButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    auto *streamMenu = new QMenu(openStreamButton);
    streamMenu->setObjectName(QStringLiteral("openStreamSplitMenu"));
    streamMenu->addAction(ui->actionOpenLogStream);
    streamMenu->addAction(ui->actionOpenNetworkStream);
    openStreamButton->setMenu(streamMenu);
    mMainToolbar->addWidget(openStreamButton);

    mMainToolbar->addSeparator();

    // Add-filter split button. Face = open the generic
    // filter editor (`actionAddFilter`'s existing slot, no
    // preselected column). Dropdown = `Add filter on "<col>"…`
    // entries, one per visible column, so a user who knows the
    // target column can land in the editor pre-pointed at it
    // without having to right-click the header section. Same
    // entry shape as the header context menu so the muscle
    // memory carries over.
    //
    // `MenuButtonPopup` (not `InstantPopup`) keeps the more-
    // common generic path one click away (it matches the
    // pre-split behaviour of the bare action) while making the
    // per-column shortcut discoverable behind the arrow.
    //
    // `setDefaultAction` also tries to install the action's
    // icon -- the split button's themed-icon refresh therefore
    // runs through `mThemedActions` (already populated for
    // `actionAddFilter` above) so a palette / theme flip
    // re-tints the button face.
    auto *addFilterButton = new QToolButton(mMainToolbar);
    addFilterButton->setObjectName(QStringLiteral("addFilterSplitButton"));
    addFilterButton->setDefaultAction(ui->actionAddFilter);
    addFilterButton->setPopupMode(QToolButton::MenuButtonPopup);
    addFilterButton->setIconSize(toolbarIconSize);
    addFilterButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    auto *addFilterMenu = new QMenu(addFilterButton);
    addFilterMenu->setObjectName(QStringLiteral("addFilterSplitMenu"));
    addFilterButton->setMenu(addFilterMenu);
    // Rebuild on every show so the listing reflects the live
    // column set without us having to invalidate it from every
    // column-mutation site (column reorder, hide/show, post-
    // stream promotion, columns-manager edit, ...). The header
    // right-click `RebuildViewMenu` uses the same idiom.
    connect(addFilterMenu, &QMenu::aboutToShow, this, [this, addFilterMenu]() { RebuildAddFilterMenu(addFilterMenu); });
    mMainToolbar->addWidget(addFilterButton);

    // Clear-filters split button. Face = `actionClearAllFilters`
    // (drop every active filter; same one-click clear the bare
    // button used to offer). Dropdown = `Remove "<col>": <title>`
    // entries, one per active filter, grouped by column index
    // then sorted by display title -- lets a user with three
    // filters drop just the misbehaving one without having to
    // dive into the Filters menu's per-filter submenu.
    //
    // `actionClearAllFilters` is gated by `setDisabled(true)`
    // when `mFilters` is empty, which on most styles disables
    // the arrow too. That's intentional: there's nothing to
    // remove either way, so the disabled arrow honestly reports
    // "nothing to do" instead of opening to a placeholder.
    auto *clearFiltersButton = new QToolButton(mMainToolbar);
    clearFiltersButton->setObjectName(QStringLiteral("clearFiltersSplitButton"));
    clearFiltersButton->setDefaultAction(ui->actionClearAllFilters);
    clearFiltersButton->setPopupMode(QToolButton::MenuButtonPopup);
    clearFiltersButton->setIconSize(toolbarIconSize);
    clearFiltersButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    auto *clearFiltersMenu = new QMenu(clearFiltersButton);
    clearFiltersMenu->setObjectName(QStringLiteral("clearFiltersSplitMenu"));
    clearFiltersButton->setMenu(clearFiltersMenu);
    connect(clearFiltersMenu, &QMenu::aboutToShow, this, [this, clearFiltersMenu]() {
        RebuildClearFiltersMenu(clearFiltersMenu);
    });
    mMainToolbar->addWidget(clearFiltersButton);

    // Sort dropdown button. The whole face opens the per-column
    // menu (`InstantPopup`); sort has no generic editor, so a
    // click-vs-arrow split would be redundant. The menu carries
    // the same per-column rows as the Sort menu, minus the
    // Clear-sort row (that lives in the dedicated button next
    // to this one).
    auto *sortByButton = new QToolButton(mMainToolbar);
    sortByButton->setObjectName(QStringLiteral("sortBySplitButton"));
    sortByButton->setDefaultAction(ui->actionSortBy);
    sortByButton->setPopupMode(QToolButton::InstantPopup);
    sortByButton->setIconSize(toolbarIconSize);
    sortByButton->setToolButtonStyle(Qt::ToolButtonIconOnly);
    auto *sortByMenu = new QMenu(sortByButton);
    sortByMenu->setObjectName(QStringLiteral("sortBySplitMenu"));
    sortByButton->setMenu(sortByMenu);
    connect(sortByMenu, &QMenu::aboutToShow, this, [this, sortByMenu]() { RebuildSortByMenu(sortByMenu); });
    mMainToolbar->addWidget(sortByButton);

    // Clear-sort plain button. Sort is single-column, so a
    // per-X dropdown would always hold one entry. Enable state
    // is driven by `UpdateSortStatus`.
    mMainToolbar->addAction(ui->actionClearSort);

    mMainToolbar->addSeparator();
    if (mActionToggleFind != nullptr)
    {
        mMainToolbar->addAction(mActionToggleFind);
    }
    mMainToolbar->addAction(ui->actionToggleRecordDetails);
    if (mActionToggleAnchors != nullptr)
    {
        mMainToolbar->addAction(mActionToggleAnchors);
    }

    // Expanding spacer pushes Preferences to the far right edge,
    // matching the "tools / settings on the right" convention used
    // by VS Code, Sublime, JetBrains, etc.
    auto *spacer = new QWidget(mMainToolbar);
    spacer->setObjectName(QStringLiteral("mainToolbarSpacer"));
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    mMainToolbar->addWidget(spacer);
    mMainToolbar->addAction(ui->actionPreferences);

    // Themed actions outside of any toolbar. The File -> Recent
    // Sessions submenu gets the `file-clock` glyph so the entry
    // is recognisable at a glance. Anchored to the window because
    // there is no host toolbar; the refresh loop falls back to
    // `PM_LargeIconSize` for sizing.
    if (ui->menuRecentSessions != nullptr)
    {
        tag(ui->menuRecentSessions->menuAction(), this, QStringLiteral(":/icons/file-clock.svg"));
    }

    // Primary-toolbar toggle action. Created once here so its
    // metadata (objectName, text) does not get rewritten on every
    // `RebuildViewMenu` (the menu rebuild only re-adds the cached
    // action to the freshly cleared menu).
    if (QAction *toggleMainToolbar = mMainToolbar->toggleViewAction(); toggleMainToolbar != nullptr)
    {
        toggleMainToolbar->setObjectName(QStringLiteral("actionToggleMainToolbar"));
        toggleMainToolbar->setText(tr("Main Toolbar"));
    }

    RefreshThemedIcons();
}

void MainWindow::RefreshThemedIcons()
{
    // Drop the model's cached funnel pixmap before the toolbar
    // null guard below: the model outlives the toolbar across
    // construction and teardown, so the funnel refresh needs to
    // run even when the toolbar leg is a no-op.
    if (mModel != nullptr)
    {
        mModel->RefreshHeaderIcons();
    }

    // Constructor-time `changeEvent` (an initial palette
    // notification can land before `BuildMainToolbar` runs) and
    // shutdown-time refreshes (Qt has already cleared the
    // `QPointer`) both reach here harmlessly via the null guard.
    if (mMainToolbar == nullptr)
    {
        return;
    }

    // Mints a `QIcon` whose Off pixmap is `offPath` and, when
    // present, On pixmap is `onPath`. The render parameters are
    // resolved once per anchor so both states share the same
    // tint / size / DPR -- otherwise the checked-state glyph
    // could land a pixel off-grid from the unchecked one when the
    // action toggles. Returns an empty QIcon if the Off SVG fails
    // to load; callers accept the text-only fallback.
    const auto buildIcon = [](const QString &offPath, const QString &onPath, const QWidget *anchor) {
        const icon_loader::IconRenderParams params = icon_loader::ResolveAnchorIconParams(anchor);
        QIcon icon = icon_loader::MakeThemedIcon(offPath, params.tint, params.sizePx, params.devicePixelRatio);
        if (icon.isNull() || onPath.isEmpty())
        {
            return icon;
        }
        const QPixmap onPixmap =
            icon_loader::MakeThemedPixmap(onPath, params.tint, params.sizePx, params.devicePixelRatio);
        if (!onPixmap.isNull())
        {
            // `QIcon::Normal / On` matches the state Qt asks for
            // when rendering a checked QAction button.
            icon.addPixmap(onPixmap, QIcon::Normal, QIcon::On);
        }
        return icon;
    };

    for (const ThemedActionEntry &entry : std::as_const(mThemedActions))
    {
        QAction *action = entry.action.data();
        if (action == nullptr)
        {
            // Action torn down out of order during shutdown; the
            // `QPointer` keeps us honest.
            continue;
        }
        const QString path = action->property("svgIconPath").toString();
        if (path.isEmpty())
        {
            // Property cleared, e.g. by a future caller swapping
            // to a non-themed icon. Leave the existing icon alone.
            continue;
        }
        const QString onPath = action->property("svgIconPathChecked").toString();
        // Anchor falls back to the window so a registered action
        // whose anchor widget has been torn down still re-tints
        // (with the window's palette / DPR) instead of silently
        // going stale.
        const QWidget *anchor = entry.anchor.data();
        action->setIcon(buildIcon(path, onPath, anchor != nullptr ? anchor : this));
    }
}

void MainWindow::RebuildAddFilterMenu(QMenu *menu)
{
    if (menu == nullptr)
    {
        return;
    }
    menu->clear();

    const auto &columns = mModel->Configuration().columns;
    if (columns.empty())
    {
        // Disabled placeholder so an empty dropdown advertises
        // *why* it is empty rather than opening as a blank box.
        // Same idiom as `RebuildViewMenu`'s `(no columns yet)`
        // sentinel.
        QAction *placeholder = menu->addAction(tr("(no columns yet)"));
        placeholder->setEnabled(false);
        return;
    }

    // `AddFilter` short-circuits with a status-bar hint when the
    // model has no rows, so disable the entries up-front rather
    // than advertise a no-op. The face button (the bare
    // `actionAddFilter`) gets the same treatment from its
    // existing `setEnabled` plumbing.
    const bool modelHasRows = mModel->rowCount() > 0;

    // Header-disambiguated labels (e.g. `name` vs `name [user|id]`)
    // so two columns sharing the same display header still produce
    // unambiguous entries -- same helper the View menu uses.
    const std::vector<QString> labels = BuildAllColumnMenuLabels();

    bool addedAny = false;
    for (size_t i = 0; i < columns.size(); ++i)
    {
        // Hidden columns are skipped to mirror the header
        // right-click menu (`SetInitialColumn` refuses to
        // preselect a hidden column, so an entry here would
        // advertise a preselection the editor would drop).
        // Re-show is delegated to the View menu, same as for
        // the header right-click.
        if (!columns[i].visible)
        {
            continue;
        }
        const QString &label = labels[i];
        QAction *act = menu->addAction(tr("Add filter on \"%1\"…").arg(label));
        act->setEnabled(modelHasRows);
        // Capture stable `keys` so a column reorder landing
        // between menu build and click still hits the right
        // column. Matches the header-context-menu lambda.
        connect(act, &QAction::triggered, this, [this, keys = columns[i].keys]() {
            const int idx = FindColumnIndexByKeys(keys);
            if (idx < 0)
            {
                return;
            }
            AddFilter(QUuid::createUuid().toString(), std::nullopt, /*openEditor=*/true, /*initialColumn=*/idx);
        });
        addedAny = true;
    }

    if (!addedAny)
    {
        // Every column hidden -- legal end state. Surface the
        // condition explicitly so the user understands why the
        // dropdown is empty (and where to re-show columns).
        QAction *placeholder = menu->addAction(tr("(every column is hidden – use View menu to show one)"));
        placeholder->setEnabled(false);
    }
}

void MainWindow::RebuildClearFiltersMenu(QMenu *menu)
{
    if (menu == nullptr)
    {
        return;
    }
    menu->clear();

    if (mFilters.empty())
    {
        QAction *placeholder = menu->addAction(tr("(no filters)"));
        placeholder->setEnabled(false);
        return;
    }

    // Disambiguated column labels (same helper the View menu /
    // Add-filter dropdown use) so two columns sharing a header
    // produce distinct entries.
    const std::vector<QString> labels = BuildAllColumnMenuLabels();

    // Flatten + sort so the menu order is deterministic.
    // `mFilters` is an unordered_map keyed by UUID, so without
    // sorting the visible order would change every time Qt's
    // hash seed changes.
    struct Entry
    {
        std::string id;
        QString columnLabel;
        QString filterTitle;
        int columnRow = -1;
    };
    std::vector<Entry> entries;
    entries.reserve(mFilters.size());
    for (const auto &[id, filter] : mFilters)
    {
        const int row = filter.row;
        QString columnLabel = (row >= 0 && static_cast<size_t>(row) < labels.size())
                                  ? labels[static_cast<size_t>(row)]
                                  // Filter pointing at a column that no
                                  // longer exists (e.g. a config carrying
                                  // over a renamed key). Surface it as
                                  // `(unknown column)` so the user can
                                  // still get rid of it via the dropdown.
                                  : tr("(unknown column)");
        entries.push_back(
            {.id = id, .columnLabel = std::move(columnLabel), .filterTitle = BuildFilterTitle(filter), .columnRow = row}
        );
    }
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) {
        // Group by column first so all filters on `level` sit
        // next to each other regardless of UUID order; secondary
        // sort by title puts e.g. `error, warn` near `info` in
        // the same column block. UUID tie-break keeps the order
        // stable across reopens.
        if (a.columnRow != b.columnRow)
        {
            return a.columnRow < b.columnRow;
        }
        const int cmp = a.filterTitle.localeAwareCompare(b.filterTitle);
        if (cmp != 0)
        {
            return cmp < 0;
        }
        return a.id < b.id;
    });

    for (const Entry &entry : entries)
    {
        const QString filterId = QString::fromStdString(entry.id);
        QAction *act = menu->addAction(tr("Remove \"%1\": %2").arg(entry.columnLabel, entry.filterTitle));
        // ObjectName carries the UUID so a test can find the
        // entry by id without parsing display text.
        act->setObjectName(filterId);
        connect(act, &QAction::triggered, this, [this, filterId]() { ClearFilter(filterId); });
    }
}

void MainWindow::ClearSort()
{
    if (mTableView == nullptr)
    {
        return;
    }
    // Same call shape `SetColumnVisible` and post-load rebuild
    // paths use, so proxy / header / config stay in lockstep
    // through one well-trodden path. No-op safe when no sort
    // is active. All UI surfaces already gate on
    // `actionClearSort`'s enabled state; the guard is for the
    // test seam and any future programmatic caller.
    mTableView->sortByColumn(-1, Qt::AscendingOrder);
}

bool MainWindow::AppendSortByEntries(QMenu *menu)
{
    if (menu == nullptr || mModel == nullptr || mSortFilterProxyModel == nullptr)
    {
        return false;
    }

    const auto &columns = mModel->Configuration().columns;
    if (columns.empty())
    {
        return false;
    }

    // Kept out of `tr()` so a translator can't alter the
    // glyphs - they must match `QHeaderView`'s sort-indicator
    // triangles. Tests pin these code points.
    static constexpr QChar SORT_ASC_GLYPH(u'\u25B2');  // ▲
    static constexpr QChar SORT_DESC_GLYPH(u'\u25BC'); // ▼

    // Disable rows when the model has no rows: a sort would be
    // a structural no-op but the action would still appear
    // available. Mirrors Add-filter's "model has rows" gate.
    const bool modelHasRows = mModel->rowCount() > 0;
    const int currentColumn = mSortFilterProxyModel->SortColumn();
    const Qt::SortOrder currentOrder = mSortFilterProxyModel->SortOrder();

    // Header-disambiguated labels (`name` vs `name [user|id]`),
    // same helper the View / Add-filter / Clear-filters menus
    // use.
    const std::vector<QString> labels = BuildAllColumnMenuLabels();

    bool addedAny = false;
    for (size_t i = 0; i < columns.size(); ++i)
    {
        // Skip hidden columns - re-show is delegated to the
        // View menu (same as the filter menus).
        if (!columns[i].visible)
        {
            continue;
        }
        const QString &label = labels[i];
        const int columnIdx = static_cast<int>(i);

        // Capture stable `keys` so a column reorder between
        // menu build and click still hits the right column.
        const auto &keys = columns[i].keys;

        const bool isActiveSortColumn = (currentColumn == columnIdx);

        // Disable Asc/Desc when the column's data doesn't
        // match its configured type: the sort would use the
        // wrong comparator and produce a misleading order.
        // The tooltip points at Configuration Diagnostics.
        const auto health = mModel->ColumnHealth(columnIdx);
        const bool typeMismatch = health.has_value() && health->presentSlots > health->matchingSlots;
        const bool ascDescEnabled = modelHasRows && !typeMismatch;
        const QString mismatchTooltip =
            tr("This column's data does not match its configured type, so sorting is disabled. "
               "Open Configuration Diagnostics to inspect or change the type.");

        // Two checkable rows per column: glyph + quoted column
        // label. The host menu's title and the triangle carry
        // the verb and direction; no "Sort by" prefix needed.
        // Only the label is translated; the glyph stays a
        // literal code-point.
        const QString quotedLabel = tr("\"%1\"").arg(label);
        const QString ascText = QString(SORT_ASC_GLYPH) + QLatin1Char(' ') + quotedLabel;
        const QString descText = QString(SORT_DESC_GLYPH) + QLatin1Char(' ') + quotedLabel;

        QAction *ascAct = menu->addAction(ascText);
        ascAct->setCheckable(true);
        ascAct->setChecked(isActiveSortColumn && currentOrder == Qt::AscendingOrder);
        ascAct->setEnabled(ascDescEnabled);
        if (typeMismatch)
        {
            ascAct->setToolTip(mismatchTooltip);
        }
        // NOLINTNEXTLINE(bugprone-exception-escape) - vector<string> capture copy can technically throw bad_alloc.
        connect(ascAct, &QAction::triggered, this, [this, keys]() {
            const int idx = FindColumnIndexByKeys(keys);
            if (idx < 0 || mTableView == nullptr)
            {
                return;
            }
            mTableView->sortByColumn(idx, Qt::AscendingOrder);
        });

        QAction *descAct = menu->addAction(descText);
        descAct->setCheckable(true);
        descAct->setChecked(isActiveSortColumn && currentOrder == Qt::DescendingOrder);
        descAct->setEnabled(ascDescEnabled);
        if (typeMismatch)
        {
            descAct->setToolTip(mismatchTooltip);
        }
        // NOLINTNEXTLINE(bugprone-exception-escape) - vector<string> capture copy can technically throw bad_alloc.
        connect(descAct, &QAction::triggered, this, [this, keys]() {
            const int idx = FindColumnIndexByKeys(keys);
            if (idx < 0 || mTableView == nullptr)
            {
                return;
            }
            mTableView->sortByColumn(idx, Qt::DescendingOrder);
        });

        addedAny = true;
    }

    if (addedAny)
    {
        // Enable per-action tooltips so the type-mismatch
        // explanation surfaces on hover (QMenu hides them by
        // default).
        menu->setToolTipsVisible(true);
    }

    return addedAny;
}

void MainWindow::AppendSortEntriesOrPlaceholder(QMenu *menu)
{
    if (menu == nullptr)
    {
        return;
    }
    if (mModel == nullptr || mModel->Configuration().columns.empty())
    {
        QAction *placeholder = menu->addAction(tr("(no columns yet)"));
        placeholder->setEnabled(false);
        return;
    }
    if (!AppendSortByEntries(menu))
    {
        // Every column hidden - surface a placeholder pointing
        // at the View menu. Same wording as
        // `RebuildAddFilterMenu`.
        QAction *placeholder = menu->addAction(tr("(every column is hidden – use View menu to show one)"));
        placeholder->setEnabled(false);
    }
}

void MainWindow::RebuildSortMenu()
{
    QMenu *menu = ui->menuSort;
    if (menu == nullptr)
    {
        return;
    }
    menu->clear();
    // Re-attach `actionClearSort` (the `clear()` above
    // detached it without destroying the action). Its enabled
    // state is already driven by `UpdateSortStatus`, so no
    // resync is needed here.
    menu->addAction(ui->actionClearSort);
    menu->addSeparator();
    AppendSortEntriesOrPlaceholder(menu);
}

void MainWindow::RebuildSortByMenu(QMenu *menu)
{
    if (menu == nullptr)
    {
        return;
    }
    menu->clear();
    AppendSortEntriesOrPlaceholder(menu);
}

void MainWindow::UpdateSortStatus()
{
    const int sortColumn = (mSortFilterProxyModel != nullptr) ? mSortFilterProxyModel->SortColumn() : -1;
    const int sourceRows = (mModel != nullptr) ? mModel->rowCount() : 0;
    const bool sortActive = sortColumn >= 0;

    if (ui != nullptr && ui->actionClearSort != nullptr)
    {
        ui->actionClearSort->setEnabled(sortActive);
    }

    if (mClearSortStatusButton == nullptr)
    {
        return;
    }
    if (sourceRows <= 0 || !sortActive)
    {
        mClearSortStatusButton->hide();
        return;
    }

    // Name the live column in the tooltip so renames and
    // reorders show through. Labels are disambiguated by
    // `[keys]` for duplicate headers.
    const std::vector<QString> labels = BuildAllColumnMenuLabels();
    const QString columnLabel =
        std::cmp_less(sortColumn, labels.size()) ? labels[static_cast<size_t>(sortColumn)] : tr("(unknown column)");
    const QString directionWord =
        (mSortFilterProxyModel->SortOrder() == Qt::DescendingOrder) ? tr("descending") : tr("ascending");
    mClearSortStatusButton->setToolTip(tr("Sorted by \"%1\" (%2) - click to clear.").arg(columnLabel, directionWord));
    mClearSortStatusButton->show();
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event == nullptr)
    {
        return;
    }
    // Light/dark theme flip changes `WindowText` -> re-mint every
    // tinted icon. `StyleChange` covers the parallel style swap a
    // theme can apply via `qApp->setStyle`. `DevicePixelRatioChange`
    // covers a drag between monitors of different DPI -- the icon's
    // backing pixmap is allocated at the current DPR and must be
    // re-rasterised at the new one. `ThemeChange` covers OS-level
    // light/dark notifications (Windows in particular) that can
    // arrive without an accompanying palette diff. `OnThemeChanged`
    // covers the application-driven switch; this hook catches the
    // OS-level events that reach the window without going through
    // `ThemeControl`.
    const QEvent::Type type = event->type();
    if (type == QEvent::PaletteChange || type == QEvent::StyleChange || type == QEvent::ApplicationPaletteChange ||
        type == QEvent::ThemeChange || type == QEvent::DevicePixelRatioChange)
    {
        RefreshThemedIcons();
    }
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
    JumpToNewestRow();
}

void MainWindow::JumpToNewestRow()
{
    if (mModel == nullptr || mRowOrderProxyModel == nullptr || mSortFilterProxyModel == nullptr ||
        mTableView == nullptr)
    {
        return;
    }
    const int sourceRowCount = mModel->rowCount();
    if (sourceRowCount <= 0)
    {
        return;
    }

    // Use the view's tail edge as the single source of truth.
    // `RowOrderProxyModel::IsReversed()` is kept in lockstep with
    // it, but the view is what the user actually sees.
    const bool tailIsTop = (mTableView->GetTailEdge() == LogTableView::TailEdge::Top);

    // Stage 1: map source-newest through the proxy chain. Lands
    // on the absolute newest line under sort + filter when it
    // survives the filter.
    const QModelIndex sourceIndex = mModel->index(sourceRowCount - 1, 0);
    const QModelIndex midIndex = mRowOrderProxyModel->mapFromSource(sourceIndex);
    QModelIndex proxyIndex = mSortFilterProxyModel->mapFromSource(midIndex);

    // Stage 2: source-newest is filtered out (common under live
    // tail with a level/error filter). Walk backwards from newest
    // and take the first source row that survives the proxy. The
    // walk is bounded so a filter excluding the entire tail can't
    // turn an O(1) jump into an O(N) GUI-thread scan.
    if (!proxyIndex.isValid())
    {
        constexpr int JUMP_FALLBACK_WALK_LIMIT = 256;
        const int maxOffset = std::min(sourceRowCount - 1, JUMP_FALLBACK_WALK_LIMIT);
        for (int offset = 1; offset <= maxOffset; ++offset)
        {
            const QModelIndex candidateSource = mModel->index(sourceRowCount - 1 - offset, 0);
            const QModelIndex candidateMid = mRowOrderProxyModel->mapFromSource(candidateSource);
            const QModelIndex candidateProxy = mSortFilterProxyModel->mapFromSource(candidateMid);
            if (candidateProxy.isValid())
            {
                proxyIndex = candidateProxy;
                break;
            }
        }
    }

    // Stage 3: snap to the proxy's visual tail so the pill click
    // always moves the viewport instead of silently doing nothing.
    if (!proxyIndex.isValid())
    {
        const int proxyRowCount = mSortFilterProxyModel->rowCount();
        if (proxyRowCount <= 0)
        {
            // Nothing visible to scroll to. Clear the pending
            // announcement -- it can't refer to any row.
            if (mTableView != nullptr)
            {
                mTableView->AcknowledgePendingNewRows();
            }
            return;
        }
        const int targetRow = tailIsTop ? 0 : (proxyRowCount - 1);
        proxyIndex = mSortFilterProxyModel->index(targetRow, 0);
        if (!proxyIndex.isValid())
        {
            return;
        }
    }

    const auto position = tailIsTop ? QAbstractItemView::PositionAtTop : QAbstractItemView::PositionAtBottom;
    mTableView->scrollTo(proxyIndex, position);
}

void MainWindow::ApplyStreamingRetention()
{
    mModel->SetRetentionCap(StreamingControl::RetentionLines());
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

    // Alternation is permanently off here -- per-level theme
    // colours already partition rows, and toggling it per
    // direction used to flicker on newest-first batches.

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
    if (mParseErrorsDock == nullptr)
    {
        // Should never hit in production (the dock is built in the
        // constructor before any open path runs). Surface so a test
        // fixture poking `ShowParseErrors` on a stripped-down window
        // doesn't lose the diagnostic silently.
        qWarning() << "ShowParseErrors: parse-errors dock is unavailable; dropping" << errors.size() << "error(s) under"
                   << title;
        return;
    }
    mParseErrorsDock->AppendErrors(title, errors);
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
    // what the user sees. *Exception:* while a deferred sort is
    // pending and the proxy is still unsorted (`-1`), preserve the
    // configuration's existing sort -- the live `-1` is transient.
    const int proxySortColumn = mSortFilterProxyModel->SortColumn();
    if (proxySortColumn >= 0 || !mPendingApplySortFromConfig)
    {
        loglib::LogConfiguration::Sort sort;
        sort.columnIndex = proxySortColumn;
        sort.descending = (mSortFilterProxyModel->SortOrder() == Qt::DescendingOrder);
        mModel->ConfigurationManager().SetSort(sort);
    }

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

    // Mirror anchors into `configuration.anchors` for both autosave
    // and manual SaveSession.
    if (mAnchors != nullptr)
    {
        mModel->ConfigurationManager().SetAnchors(mAnchors->Entries());
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

    // Persist geometry/dock layout before tear-down. Best-effort: a
    // QSettings write failure is silently swallowed alongside the
    // auto-save failures below.
    SaveWindowChrome();

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
    const QString file = QFileDialog::getSaveFileName(
        this, tr("Save Configuration"), DefaultOpenDir(), tr("JSON (*.json);;All Files (*)")
    );
    if (file.isEmpty())
    {
        return;
    }
    RememberLastOpenDir(file);
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
    const QString file =
        QFileDialog::getSaveFileName(this, tr("Save Session"), DefaultOpenDir(), tr("JSON (*.json);;All Files (*)"));
    if (file.isEmpty())
    {
        return;
    }
    RememberLastOpenDir(file);
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
    // Save succeeded — runtime now matches disk, so drop `[*]`.
    // A throw above (correctly) skips this and leaves the marker set.
    mFiltersDirty = false;
    UpdateWindowTitle();
}

void MainWindow::LoadConfiguration()
{
    const QString file = QFileDialog::getOpenFileName(
        this, tr("Load Configuration"), DefaultOpenDir(), tr("JSON (*.json);;All Files (*)")
    );
    if (file.isEmpty())
    {
        return;
    }
    RememberLastOpenDir(file);
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
    // `isHidden()` probes the dock's own explicit-hide flag; the
    // ancestor `isVisible()` is also false until `show()` has been
    // called on the host. The `isVisible()` guard on `this` is
    // defense-in-depth for unit tests that drive this slot without
    // realising the main window: `QDockWidget::setVisible(true)`
    // walks `QMainWindowLayout`'s dock-area state, which is only
    // wired up by the host's first paint cycle. Production callers
    // always see a visible main window.
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

void MainWindow::UpdateParseErrorsStatus(int count, int droppedCount)
{
    if (mParseErrorsStatusButton == nullptr)
    {
        return;
    }
    if (count <= 0 && droppedCount <= 0)
    {
        mParseErrorsStatusButton->hide();
        mParseErrorsStatusButton->setText(QString());
        mParseErrorsStatusButton->setToolTip(QString());
        return;
    }
    // `%Ln` -> locale-grouped digits matching the streaming-status
    // text (no width jitter as counts grow).
    const int displayedTotal = count + droppedCount;
    if (droppedCount > 0)
    {
        // Surface the dropped-count on the label itself; otherwise
        // the button says "12,345 parse errors" but the dock reads
        // "11,345 errors; 1,000 earlier dropped" -- looks like a bug.
        const QLocale locale = QLocale::system();
        mParseErrorsStatusButton->setText(tr("%1 parse error(s) (+%2 dropped)")
                                              .arg(locale.toString(static_cast<qlonglong>(count)))
                                              .arg(locale.toString(static_cast<qlonglong>(droppedCount))));
        // Two independent counts -> can't use a single `%Ln` plural.
        mParseErrorsStatusButton->setToolTip(
            tr("%1 parse error(s) in this session "
               "(%2 visible, %3 earlier dropped). Click to open the Parse Errors panel.")
                .arg(locale.toString(static_cast<qlonglong>(displayedTotal)))
                .arg(locale.toString(static_cast<qlonglong>(count)))
                .arg(locale.toString(static_cast<qlonglong>(droppedCount)))
        );
    }
    else
    {
        mParseErrorsStatusButton->setText(tr("%Ln parse error(s)", nullptr, displayedTotal));
        mParseErrorsStatusButton->setToolTip(
            tr("%Ln parse error(s) in this session. Click to open the Parse Errors panel.", nullptr, count)
        );
    }
    mParseErrorsStatusButton->show();
}

void MainWindow::UpdateFindMatchCount(const QString &text, bool wildcards, bool regularExpressions)
{
    if (mFindRecord == nullptr || mSortFilterProxyModel == nullptr)
    {
        return;
    }
    // Skip the full-table scan when the bar isn't visible; a debounce
    // timer armed before the dismissal can fire after the fact, and
    // refreshing an off-screen indicator is pointless.
    if (!IsFindBarVisible())
    {
        return;
    }
    if (text.isEmpty())
    {
        // Only reachable via a programmatic call (the widget
        // suppresses the signal on empty text). Keep the label
        // clear in case the cache was previously populated.
        InvalidateFindMatchCache();
        mFindRecord->SetMatchInfo(0, 0);
        return;
    }

    // Rebuild only when the needle / flags actually changed. A
    // Next / Previous click reports the same needle, so the second
    // call just resolves the new `i` via binary search below.
    const bool cacheHit = mFindMatchCache.has_value() && mFindMatchCache->needle == text &&
                          mFindMatchCache->wildcards == wildcards &&
                          mFindMatchCache->regularExpressions == regularExpressions;
    if (!cacheHit)
    {
        const Qt::MatchFlags flags = LogFilterModel::ComposeFindFlags(wildcards, regularExpressions);
        const QModelIndex start = mSortFilterProxyModel->index(0, 0);
        if (!start.isValid())
        {
            InvalidateFindMatchCache();
            mFindRecord->SetMatchInfo(0, 0);
            return;
        }
        const QVariant value = QVariant::fromValue(text);
        // Cap + 1: the extra hit lets us distinguish "exactly at the
        // cap" from "ran off the end". Keeps the GUI bounded on huge
        // tables with a common needle.
        const QModelIndexList matches =
            mSortFilterProxyModel->MatchRow(start, Qt::DisplayRole, value, MAX_FIND_MATCH_COUNT + 1, flags, true, 0);
        const bool overflowed = matches.size() > MAX_FIND_MATCH_COUNT;
        // `MatchRow` is contracted to return one entry per row in
        // ascending order. Sort + unique defensively so a future
        // contract drift can't silently corrupt the binary search;
        // asserts catch the regression at the source in debug builds.
        std::vector<int> rows;
        rows.reserve(static_cast<size_t>(matches.size()));
        for (const QModelIndex &m : matches)
        {
            rows.push_back(m.row());
        }
        // Sort check must precede dedup check: `adjacent_find` only
        // spots adjacent dupes, so unsorted input with non-adjacent
        // duplicates would slip through. `qWarning` mirrors the
        // assert because release builds compile it out.
        const bool sortedAsExpected = std::ranges::is_sorted(rows);
        Q_ASSERT(sortedAsExpected);
        if (!sortedAsExpected)
        {
            qWarning() << "MainWindow::UpdateFindMatchCount: MatchRow returned unsorted rows; "
                          "sorting defensively. This is a contract violation worth investigating.";
            std::ranges::sort(rows);
        }
        const bool dedupedAsExpected = std::ranges::adjacent_find(rows) == rows.end();
        Q_ASSERT(dedupedAsExpected);
        if (!dedupedAsExpected)
        {
            qWarning() << "MainWindow::UpdateFindMatchCount: MatchRow returned duplicate rows; "
                          "deduplicating defensively. This is a contract violation worth investigating.";
        }
        rows.erase(std::ranges::unique(rows).begin(), rows.end());
        // Trim only when there's surplus -- the dedup above can drop
        // entries below the cap, and a blind `resize(cap)` would then
        // grow the vector with zeros that look like matches at row 0.
        if (rows.size() > static_cast<size_t>(MAX_FIND_MATCH_COUNT))
        {
            rows.resize(static_cast<size_t>(MAX_FIND_MATCH_COUNT));
        }
        mFindMatchCache = FindMatchCache{
            .needle = text,
            .wildcards = wildcards,
            .regularExpressions = regularExpressions,
            .overflowed = overflowed,
            .sortedRows = std::move(rows),
        };
    }

    const int total = static_cast<int>(mFindMatchCache->sortedRows.size());
    int currentOneBased = 0;
    if (total > 0 && mTableView != nullptr)
    {
        const QModelIndex currentIdx = mTableView->currentIndex();
        if (currentIdx.isValid())
        {
            const auto begin = mFindMatchCache->sortedRows.begin();
            const auto end = mFindMatchCache->sortedRows.end();
            const auto it = std::lower_bound(begin, end, currentIdx.row());
            if (it != end && *it == currentIdx.row())
            {
                currentOneBased = static_cast<int>(it - begin) + 1;
            }
        }
    }
    mFindRecord->SetMatchInfo(currentOneBased, total, mFindMatchCache->overflowed);
}

void MainWindow::InvalidateFindMatchCache()
{
    mFindMatchCache.reset();
}

void MainWindow::OnFindCacheInvalidated()
{
    InvalidateFindMatchCache();
    if (mFindRecord != nullptr && IsFindBarVisible())
    {
        mFindRecord->BumpMatchCountDebounce();
    }
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
        // Config load is a session boundary; re-arm the auto-raise
        // and reset the per-file slice watermark.
        if (mParseErrorsDock != nullptr)
        {
            mParseErrorsDock->ResetSessionState();
        }
        mStreamingErrorsCut = 0;
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
        //
        // *Exception*: when `mPendingApplySortFromConfig` is set
        // (streaming will follow), skip the eager apply -- see
        // `ApplyDeferredSortFromConfig` for the O(N^2) avoidance.
        if (!mPendingApplySortFromConfig)
        {
            const auto loadedSort = mModel->Configuration().sort;
            if (loadedSort.columnIndex >= 0 && loadedSort.columnIndex < mModel->columnCount())
            {
                mTableView->sortByColumn(
                    loadedSort.columnIndex, loadedSort.descending ? Qt::DescendingOrder : Qt::AscendingOrder
                );
            }
        }

        // Mirror the loaded source so the next save round-trips
        // it; no auto-bind (foreign sessions would be hostile).
        // Backfill the parallel dedup-keys array for older JSON.
        mCurrentSource = mModel->Configuration().source;
        logapp::BackfillLocatorDedupKeys(mCurrentSource);

        // Bulk-replace anchors from the loaded vector. Future-schema
        // colour slots are reported back so the user knows about
        // anchors that didn't survive.
        if (mAnchors != nullptr)
        {
            const std::size_t droppedAnchorCount = mAnchors->Replace(mModel->Configuration().anchors);
            if (droppedAnchorCount > 0)
            {
                statusBar()->showMessage(
                    tr("%1 anchor(s) from a newer schema were dropped.")
                        .arg(static_cast<qulonglong>(droppedAnchorCount)),
                    STATUS_BAR_MESSAGE_TIMEOUT_MS
                );
            }
        }

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

    // Suppress per-filter dirty/title updates; emit one consolidated state
    // on scope exit.
    mLoadingConfiguration = true;
    const auto guard = qScopeGuard([this]() {
        mLoadingConfiguration = false;
        // Loaded set matches disk, so start clean.
        mFiltersDirty = false;
        UpdateWindowTitle();
    });

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
    SyncColumnFilterIndicators();

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
    if (mFindDock == nullptr)
    {
        return;
    }
    // Smart toggle (VS Code / Chrome convention):
    //   - hidden / tab-buried        -> reveal + focus
    //   - visible, focus outside     -> focus the field (no close)
    //   - visible, focus inside      -> close (Ctrl+F is also the
    //                                   dismiss verb -- no chasing Esc)
    if (FindBarHoldsFocus())
    {
        mFindDock->close();
        return;
    }
    mFindDock->RevealAndFocus();
}

void MainWindow::SelectSourceRow(int sourceRow)
{
    if (mTableView == nullptr || mModel == nullptr || mRowOrderProxyModel == nullptr ||
        mSortFilterProxyModel == nullptr)
    {
        return;
    }
    if (sourceRow < 0 || sourceRow >= mModel->rowCount())
    {
        // Caller's row is stale (evicted or session swap). Surface
        // it instead of silently no-oping.
        statusBar()->showMessage(tr("Row is not currently visible."), STATUS_BAR_MESSAGE_TIMEOUT_MS);
        return;
    }
    const QModelIndex sourceIdx = mModel->index(sourceRow, 0);
    const QModelIndex midIdx = mRowOrderProxyModel->mapFromSource(sourceIdx);
    if (!midIdx.isValid())
    {
        statusBar()->showMessage(tr("Row is not currently visible."), STATUS_BAR_MESSAGE_TIMEOUT_MS);
        return;
    }
    const QModelIndex proxyIdx = mSortFilterProxyModel->mapFromSource(midIdx);
    if (!proxyIdx.isValid())
    {
        statusBar()->showMessage(tr("Row is not currently visible."), STATUS_BAR_MESSAGE_TIMEOUT_MS);
        return;
    }

    mTableView->clearSelection();
    // Centre so the user sees context around the anchor.
    mTableView->scrollTo(proxyIdx, QAbstractItemView::PositionAtCenter);
    mTableView->selectionModel()->select(proxyIdx, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    mTableView->selectionModel()->setCurrentIndex(proxyIdx, QItemSelectionModel::NoUpdate);
}

void MainWindow::JumpToAnchor(bool forward)
{
    if (mAnchors == nullptr || mTableView == nullptr || mModel == nullptr || mRowOrderProxyModel == nullptr ||
        mSortFilterProxyModel == nullptr)
    {
        return;
    }
    if (mAnchors->Empty())
    {
        statusBar()->showMessage(tr("No anchors set."), STATUS_BAR_MESSAGE_TIMEOUT_MS);
        return;
    }

    const QAbstractItemModel *proxyModel = mTableView->model();
    if (proxyModel == nullptr)
    {
        return;
    }
    const int proxyRowCount = proxyModel->rowCount();
    if (proxyRowCount <= 0)
    {
        statusBar()->showMessage(tr("No anchored rows are currently visible."), STATUS_BAR_MESSAGE_TIMEOUT_MS);
        return;
    }

    // Enumerate anchors rather than walking every proxy row: the
    // anchor count is bounded by user clicks while the proxy can
    // be tens of thousands of rows deep on a streaming session.
    const auto anchorEntries = mAnchors->Entries();
    std::vector<int> anchoredProxyRows;
    anchoredProxyRows.reserve(anchorEntries.size());
    for (const auto &entry : anchorEntries)
    {
        const AnchorManager::Key key{.locator = entry.locator, .lineId = entry.lineId};
        const int sourceRow = mModel->SourceRowForAnchorKey(key);
        if (sourceRow < 0)
        {
            // Anchor outlived its row.
            continue;
        }
        const QModelIndex sourceIdx = mModel->index(sourceRow, 0);
        const QModelIndex midIdx = mRowOrderProxyModel->mapFromSource(sourceIdx);
        if (!midIdx.isValid())
        {
            continue;
        }
        const QModelIndex proxyIdx = mSortFilterProxyModel->mapFromSource(midIdx);
        if (!proxyIdx.isValid())
        {
            // Anchor is filtered out.
            continue;
        }
        anchoredProxyRows.push_back(proxyIdx.row());
    }

    if (anchoredProxyRows.empty())
    {
        // Anchors exist but every one is filtered out.
        statusBar()->showMessage(tr("No anchored rows are currently visible."), STATUS_BAR_MESSAGE_TIMEOUT_MS);
        return;
    }

    // Sort into proxy-row order so next/previous match what the
    // user sees, not insertion / lineId order.
    std::ranges::sort(anchoredProxyRows);
    // Dedup so cross-file lineId collisions count as one stop.
    anchoredProxyRows.erase(std::ranges::unique(anchoredProxyRows).begin(), anchoredProxyRows.end());

    // Use the current index (survives Ctrl-click selection moves);
    // with no current index, start before / past the visible range
    // so the first step lands on the first / last anchored row.
    int currentProxyRow = -1;
    if (const QModelIndex curProxy = mTableView->currentIndex(); curProxy.isValid())
    {
        currentProxyRow = curProxy.row();
    }
    if (currentProxyRow < 0)
    {
        currentProxyRow = forward ? -1 : proxyRowCount;
    }

    int targetProxyRow = -1;
    if (forward)
    {
        // First anchor strictly past the cursor.
        const auto it = std::ranges::upper_bound(anchoredProxyRows, currentProxyRow);
        targetProxyRow = (it != anchoredProxyRows.end()) ? *it : anchoredProxyRows.front();
    }
    else
    {
        // Last anchor strictly before the cursor.
        const auto it = std::ranges::lower_bound(anchoredProxyRows, currentProxyRow);
        targetProxyRow = (it != anchoredProxyRows.begin()) ? *(it - 1) : anchoredProxyRows.back();
    }

    const QModelIndex proxyIdx = proxyModel->index(targetProxyRow, 0);
    if (!proxyIdx.isValid())
    {
        return;
    }

    mTableView->clearSelection();
    mTableView->scrollTo(proxyIdx, QAbstractItemView::PositionAtCenter);
    mTableView->selectionModel()->select(proxyIdx, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    mTableView->selectionModel()->setCurrentIndex(proxyIdx, QItemSelectionModel::NoUpdate);
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
    // Match-type flags are alternatives, not additions; OR-ing
    // contains with regex / wildcard silently demotes the search.
    // `ComposeFindFlags` is the single source of truth shared with
    // `UpdateFindMatchCount`.
    const Qt::MatchFlags flags = LogFilterModel::ComposeFindFlags(wildcards, regularExpressions);
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

    // Refresh the "i of N" indicator now that the current index moved.
    // Cache-hit resolves the new `i` via binary search; gated on
    // visibility so a programmatic call from tests pays nothing.
    if (IsFindBarVisible())
    {
        UpdateFindMatchCount(text, wildcards, regularExpressions);
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

    auto *filterEditor = new FilterEditor(*mModel, filterId, mTheme, this);
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
    MarkFiltersDirty();
    SyncColumnFilterIndicators();
}

void MainWindow::ClearFilter(const QString &filterID, bool deferSync)
{
    mFilters.erase(filterID.toStdString());
    if (!deferSync)
    {
        MirrorSessionStateToConfiguration();
        UpdateFilters();
    }
    MarkFiltersDirty();

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

    if (!deferSync)
    {
        SyncColumnFilterIndicators();
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
    // Every user-driven filter mutation funnels through here, so one
    // mark-dirty covers them all. Config reloads are silenced by the guard.
    MarkFiltersDirty();

    const QString title = BuildFilterTitle(filter);

    QMenu *menuItem = ui->menuFilters->addMenu(title);
    menuItem->setObjectName(id);
    menuItem->menuAction()->setData(QVariant(id));

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

    // Mirror the deferSync gating used for
    // `MirrorSessionStateToConfiguration` / `UpdateFilters`: bulk
    // callers run a single trailing sync after their loop.
    if (!deferSync)
    {
        SyncColumnFilterIndicators();
    }
}

void MainWindow::SyncColumnFilterIndicators()
{
    if (mModel == nullptr)
    {
        return;
    }
    const int cols = mModel->columnCount();
    std::vector<QStringList> perColumnTitles;
    if (cols > 0)
    {
        perColumnTitles.resize(static_cast<size_t>(cols));
        for (const auto &[id, filter] : mFilters)
        {
            if (filter.row < 0 || filter.row >= cols)
            {
                // Stale row from a concurrent column drop; the
                // next filter mutation will remap or evict it.
                continue;
            }
            perColumnTitles[static_cast<size_t>(filter.row)].append(BuildFilterTitle(filter));
        }
        // Sort titles so tooltip ordering is stable across
        // `mFilters`'s unordered iteration. Use `QCollator` for
        // locale-aware, case-insensitive ordering with numeric
        // mode (so "9" sorts before "10").
        QCollator collator;
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        collator.setNumericMode(true);
        for (auto &titles : perColumnTitles)
        {
            if (titles.size() > 1)
            {
                std::sort(titles.begin(), titles.end(), [&collator](const QString &a, const QString &b) {
                    return collator.compare(a, b) < 0;
                });
            }
        }
    }
    mModel->SetColumnFilterDetails(std::move(perColumnTitles));
}

void MainWindow::OnThemeChanged()
{
    // Clear the "last applied" tracker so `ApplyTableStyleSheet` re-runs
    // the polish cascade. `QStyleSheetStyle` caches palette-derived
    // colours at polish time, and our "skip unchanged writes" guard
    // would otherwise leave the cache frozen on the old theme.
    mLastBodyStyleSheet = QString{};
    mLastHeaderStyleSheet = QString{};
    ApplyTableStyleSheet();
    ApplyThemedWindowIcon();

    // Re-query brushes for cells whose `data(BackgroundRole)` returns
    // an explicit colour (Error / Warn / anchor); the QSS polish above
    // only covers palette-default cells.
    if (mModel != nullptr)
    {
        mModel->RefreshAllRowStyles();
    }
    if (mTableView != nullptr)
    {
        // Headers don't go through `data()`, so the emit above doesn't
        // reach them. Repaint also flushes any backing-store fragments
        // left by a modal dialog (e.g. Preferences) that was overlapping.
        mTableView->viewport()->update();
        mTableView->horizontalHeader()->update();
        mTableView->verticalHeader()->update();
    }

    // These widgets cache palette-derived state (e.g. brushes
    // stamped on table items) and won't update from a bare
    // `ApplicationPaletteChange` alone.
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

    // Re-tint the Lucide icons so a Light <-> Dark flip keeps them
    // visible. `themeChanged` is the in-app entry point and can
    // land without an event-loop palette change (e.g. a Force-mode
    // toggle that pins the same OS scheme). Also drops the model's
    // cached funnel pixmap so the header decoration re-renders.
    RefreshThemedIcons();

    // A theme switch can flip icon mode on/off; reapply so the
    // delegate is attached/detached on the right column. Explicit
    // detach in text mode avoids routing every paint through the
    // delegate's self-gate.
    ApplyLevelCellDelegate();
}

void MainWindow::ApplyThemedWindowIcon()
{
    // Drive the icon off `ThemeKind` (not the OS palette) so the
    // icon matches the theme even in Force mode. No-theme test
    // path defaults to the light-OS icon.
    const loglib::ThemeKind kind = (mTheme != nullptr) ? mTheme->Active().kind : loglib::ThemeKind::Light;
    const QString iconPath =
        (kind == loglib::ThemeKind::Light) ? QStringLiteral(":/icon-black.png") : QStringLiteral(":/icon-white.png");
    setWindowIcon(QIcon(iconPath));
}

void MainWindow::ApplyTableStyleSheet()
{
    // Body chrome comes from `ThemeControl::ApplyTheme`'s palette. The only
    // body rule we need is a monospace family for log cells, scoped to
    // `QTableView::item` so the widget's font metrics — which Qt uses to
    // size scrollbars/rows/headers — stay on the system default. Keeps the
    // `TestTailEdgeTopFollowsScrollbarMinimum` scenario intact and matches
    // the family used by the raw-JSON pane. Skipped when the theme pins
    // `app.fontFamily` so the user's choice wins end-to-end.
    QString bodyRule;
    const bool themeOverridesFont =
        mTheme != nullptr && mTheme->Active().app.fontFamily.has_value() && !mTheme->Active().app.fontFamily->empty();
    if (!themeOverridesFont)
    {
        const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        const QStringList families = mono.families();
        if (!families.isEmpty())
        {
            // Quote each family so names with spaces (e.g. "Cascadia Mono")
            // parse correctly inside the QSS list.
            QStringList quoted;
            quoted.reserve(families.size());
            for (const QString &fam : families)
            {
                quoted.append(QStringLiteral("\"%1\"").arg(fam));
            }
            bodyRule = QStringLiteral("QTableView::item { font-family: %1; }").arg(quoted.join(QStringLiteral(", ")));
        }
    }

    QString headerRule = QStringLiteral("QHeaderView::section { padding: 8px; font-weight: bold;");
    if (mTheme != nullptr)
    {
        const loglib::Theme &theme = mTheme->Active();
        if (theme.table.headerBackground.has_value() && !theme.table.headerBackground->empty())
        {
            headerRule +=
                QStringLiteral(" background-color: %1;").arg(QString::fromStdString(*theme.table.headerBackground));
        }
        if (theme.table.headerForeground.has_value() && !theme.table.headerForeground->empty())
        {
            headerRule += QStringLiteral(" color: %1;").arg(QString::fromStdString(*theme.table.headerForeground));
        }
    }
    headerRule += QStringLiteral(" }");

    // Skip unchanged writes -- even an empty `setStyleSheet`
    // triggers Qt's full polish cascade.
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

void MainWindow::ApplyDeferredSortFromConfig()
{
    // Always clear the latch so subsequent saves read the proxy's
    // live sort instead of preserving the loaded one.
    const auto guard = qScopeGuard([this]() { mPendingApplySortFromConfig = false; });
    if (!mPendingApplySortFromConfig)
    {
        return;
    }
    // User sorted mid-stream -- their choice wins.
    if (mSortFilterProxyModel->SortColumn() >= 0)
    {
        return;
    }
    const auto &cfgSort = mModel->Configuration().sort;
    if (cfgSort.columnIndex < 0 || cfgSort.columnIndex >= mModel->columnCount())
    {
        return;
    }
    mTableView->sortByColumn(cfgSort.columnIndex, cfgSort.descending ? Qt::DescendingOrder : Qt::AscendingOrder);
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
    //
    // The trailing `SyncColumnFilterIndicators` inside
    // `ApplyColumnVisibility` picks up the new section indices.
    // Syncing earlier could flash the funnel on the wrong column
    // while hidden flags are still mid-flight.
    ApplyColumnVisibility();

    // Reapply *after* the filter remap above so the delegate
    // reapply sees a consistent filter store. Cheap when nothing
    // changed -- early-out on `mInstalledLevelDelegateColumn ==
    // newColumn`.
    ApplyLevelCellDelegate();
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
        filterSubMenu->setObjectName(filterId);
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

    // Sort block: `Sort ascending|descending by "<col>"` and
    // `Clear sort`, contextualised to the clicked column.
    // Hidden columns are skipped (production right-clicks
    // always hit a visible section).
    if (thisColumn.visible)
    {
        if (!menu->isEmpty())
        {
            menu->addSeparator();
        }
        const int currentSortColumn = (mSortFilterProxyModel != nullptr) ? mSortFilterProxyModel->SortColumn() : -1;
        const Qt::SortOrder currentSortOrder =
            (mSortFilterProxyModel != nullptr) ? mSortFilterProxyModel->SortOrder() : Qt::AscendingOrder;

        // Same type-mismatch gate as the Sort menu - a
        // mismatched column would sort via the wrong comparator
        // and mislead. The header tooltip already exposes the
        // diagnostic.
        const auto sortHealth = mModel->ColumnHealth(logicalColumn);
        const bool sortTypeMismatch = sortHealth.has_value() && sortHealth->presentSlots > sortHealth->matchingSlots;
        const bool sortAscDescEnabled = modelHasRows && !sortTypeMismatch;
        const QString sortMismatchTooltip =
            tr("This column's data does not match its configured type, so sorting is disabled. "
               "Open Configuration Diagnostics to inspect or change the type.");
        // Enable per-action tooltips so the type-mismatch
        // explanation surfaces on hover.
        menu->setToolTipsVisible(true);

        QAction *sortAscAction = menu->addAction(tr("Sort ascending by \"%1\"").arg(thisLabel));
        sortAscAction->setCheckable(true);
        sortAscAction->setChecked(currentSortColumn == logicalColumn && currentSortOrder == Qt::AscendingOrder);
        sortAscAction->setEnabled(sortAscDescEnabled);
        if (sortTypeMismatch)
        {
            sortAscAction->setToolTip(sortMismatchTooltip);
        }
        connect(sortAscAction, &QAction::triggered, this, [this, keys = thisKeys]() {
            const int idx = FindColumnIndexByKeys(keys);
            if (idx < 0 || mTableView == nullptr)
            {
                return;
            }
            mTableView->sortByColumn(idx, Qt::AscendingOrder);
        });

        QAction *sortDescAction = menu->addAction(tr("Sort descending by \"%1\"").arg(thisLabel));
        sortDescAction->setCheckable(true);
        sortDescAction->setChecked(currentSortColumn == logicalColumn && currentSortOrder == Qt::DescendingOrder);
        sortDescAction->setEnabled(sortAscDescEnabled);
        if (sortTypeMismatch)
        {
            sortDescAction->setToolTip(sortMismatchTooltip);
        }
        connect(sortDescAction, &QAction::triggered, this, [this, keys = thisKeys]() {
            const int idx = FindColumnIndexByKeys(keys);
            if (idx < 0 || mTableView == nullptr)
            {
                return;
            }
            mTableView->sortByColumn(idx, Qt::DescendingOrder);
        });

        // Re-attach the shared `actionClearSort` so the header
        // menu inherits its text, enabled state, tooltip, and
        // any future shortcut / icon - one source of truth
        // across every Sort surface. The host menu is built
        // fresh per right-click and `deleteLater`d on dismiss,
        // so re-attaching is safe.
        if (ui->actionClearSort != nullptr)
        {
            menu->addAction(ui->actionClearSort);
        }
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

    // Right-click on a row outside the selection collapses to that
    // row (Explorer / Excel idiom) so the Anchor sub-menu's state
    // and actions agree. Right-click inside the selection keeps the
    // multi-row set intact.
    if (QItemSelectionModel *selectionModel = mTableView->selectionModel(); selectionModel != nullptr)
    {
        if (!selectionModel->isRowSelected(proxyIndex.row(), proxyIndex.parent()))
        {
            selectionModel->select(
                proxyIndex,
                QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows | QItemSelectionModel::Current
            );
            selectionModel->setCurrentIndex(proxyIndex, QItemSelectionModel::NoUpdate);
        }
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

    auto *menu = new QMenu(parent != nullptr ? parent : mTableView);

    // Anchor section is always present and always first.
    AppendAnchorActionsToRowMenu(menu, sourceRow);

    // Pin to the first time column (shared with the Record Details
    // summary via `FirstTimeColumnIndex`).
    const auto &config = mModel->Configuration();
    const auto &columns = config.columns;
    const int timeCol = loglib::FirstTimeColumnIndex(config);
    const std::optional<int64_t> micros =
        timeCol >= 0 ? loglib::AsEpochMicroseconds(
                           mModel->Table().GetValue(static_cast<size_t>(sourceRow), static_cast<size_t>(timeCol))
                       )
                     : std::nullopt;
    if (micros.has_value())
    {
        if (!menu->isEmpty())
        {
            menu->addSeparator();
        }

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
    }

    // The anchor sub-menu is always added, so `menu` is non-empty.
    return menu;
}

void MainWindow::AppendAnchorActionsToRowMenu(QMenu *menu, int sourceRow)
{
    if (menu == nullptr || mAnchors == nullptr || mTheme == nullptr || mModel == nullptr)
    {
        return;
    }

    auto *anchorMenu = menu->addMenu(tr("Anchor"));

    // Check state reflects the right-clicked row; triggered actions
    // operate on the current selection (same path as Ctrl+1..8).
    const auto rightClickedKey = mModel->AnchorKeyForRow(sourceRow);
    const auto currentColour = rightClickedKey.has_value() ? mAnchors->ColorFor(*rightClickedKey) : std::nullopt;

    // Swatch size from the active style so icons scale with HiDPI.
    constexpr int SWATCH_ICON_FALLBACK_PX = 16;
    int swatchPx = SWATCH_ICON_FALLBACK_PX;
    if (const QStyle *windowStyle = style(); windowStyle != nullptr)
    {
        const int metric = windowStyle->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
        if (metric > 0)
        {
            swatchPx = metric;
        }
    }
    constexpr qreal SWATCH_PAINT_INSET = 0.5;
    constexpr qreal SWATCH_CORNER_RADIUS = 3.0;
    auto makeSwatchIcon = [this, swatchPx](int colorIndex) -> QIcon {
        const QBrush bg = mTheme->AnchorBrushFor(static_cast<std::uint8_t>(colorIndex), Qt::BackgroundRole);
        const QBrush fg = mTheme->AnchorBrushFor(static_cast<std::uint8_t>(colorIndex), Qt::ForegroundRole);
        QPixmap pix(swatchPx, swatchPx);
        pix.fill(Qt::transparent);
        QPainter painter(&pix);
        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(bg);
        painter.setPen(QPen(fg.color(), 1));
        painter.drawRoundedRect(
            QRectF(SWATCH_PAINT_INSET, SWATCH_PAINT_INSET, swatchPx - 1, swatchPx - 1),
            SWATCH_CORNER_RADIUS,
            SWATCH_CORNER_RADIUS
        );
        return QIcon{pix};
    };

    // No `setShortcut` here: `mAnchorColorActions[i]` already owns
    // the window-level chord, and duplicating it would trip Qt's
    // `ambiguousShortcut` warning while the popup is mapped.
    const int currentColourIndex = currentColour.has_value() ? static_cast<int>(*currentColour) : -1;
    for (std::size_t i = 0; i < loglib::ANCHOR_PALETTE_SIZE; ++i)
    {
        const int colourIndex = static_cast<int>(i);
        QAction *action = anchorMenu->addAction(makeSwatchIcon(colourIndex), tr("Colour %1").arg(colourIndex + 1));
        action->setCheckable(true);
        action->setChecked(currentColourIndex == colourIndex);
        connect(action, &QAction::triggered, mTableView, [view = mTableView, colourIndex]() {
            view->AnchorSelection(colourIndex);
        });
    }
    anchorMenu->addSeparator();
    QAction *clearAction = anchorMenu->addAction(tr("Remove anchor"));
    clearAction->setEnabled(rightClickedKey.has_value() && currentColour.has_value());
    connect(clearAction, &QAction::triggered, mTableView, &LogTableView::ClearAnchorOnSelection);
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
    // `MatchRow` honours `Column::visible`, but visibility flips
    // don't emit any of the signals `OnFindCacheInvalidated` listens
    // to. Invalidate explicitly so the indicator can't strand a
    // count that still includes hits from hidden columns.
    OnFindCacheInvalidated();
    // Hide/show doesn't change `filter.row`, so this sync is
    // usually a model-side no-op. Kept for symmetry with
    // `ApplyColumnVisibility`.
    SyncColumnFilterIndicators();
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
    // Visibility may have changed without a signal -- this is also
    // called from header-recovery and configuration-load paths. Drop
    // the find cache for the same reason as `SetColumnVisible`.
    OnFindCacheInvalidated();
    // See `SetColumnVisible`: usually a no-op, kept for symmetry
    // across column-shape signal points.
    SyncColumnFilterIndicators();
}

void MainWindow::ApplyLevelCellDelegate()
{
    // No-theme test fixture: no delegate, no icon mode.
    if (mLevelCellDelegate == nullptr || mTableView == nullptr || mModel == nullptr)
    {
        return;
    }

    // Detach in text mode (don't just rely on the delegate's
    // self-gate) so text-mode paints skip the proxy-chain walk
    // inside the delegate.
    const bool iconMode = mModel->IsLevelIconModeActive();
    const int newColumn = iconMode ? mModel->FirstLevelColumnIndex() : -1;

    // Detach from the previous column when the level column has
    // moved -- otherwise the delegate would suppress text on the
    // old column after a reload.
    if (mInstalledLevelDelegateColumn >= 0 && mInstalledLevelDelegateColumn != newColumn)
    {
        // `nullptr` reverts to the default delegate; Qt keeps
        // ownership of `mLevelCellDelegate` via this `MainWindow`.
        mTableView->setItemDelegateForColumn(mInstalledLevelDelegateColumn, nullptr);
        mInstalledLevelDelegateColumn = -1;
    }

    if (newColumn < 0)
    {
        return;
    }

    if (mInstalledLevelDelegateColumn == newColumn)
    {
        return;
    }

    mTableView->setItemDelegateForColumn(newColumn, mLevelCellDelegate);
    mInstalledLevelDelegateColumn = newColumn;
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

    // Anchors dock toggle. Programmatic action (not in main_window.ui)
    // so it has to be re-added on every rebuild -- the menu is cleared
    // above.
    if (mActionToggleAnchors != nullptr)
    {
        viewMenu->addAction(mActionToggleAnchors);
    }

    // Find + parse-errors dock toggles, re-added on every rebuild
    // (same pattern as `mActionToggleAnchors`).
    if (mActionToggleFind != nullptr)
    {
        viewMenu->addAction(mActionToggleFind);
    }
    if (mActionToggleParseErrors != nullptr)
    {
        viewMenu->addAction(mActionToggleParseErrors);
    }

    // Primary toolbar toggle. `QToolBar::toggleViewAction` returns a
    // cached checkable action whose state mirrors `QToolBar::isVisible()`
    // and which Qt keeps in sync without further wiring -- toggling
    // hides the toolbar and the user has a discoverable way to bring
    // it back. Metadata (objectName, text) was set once in
    // `BuildMainToolbar`; we only need to re-add the action to the
    // freshly cleared menu here. We deliberately don't expose
    // `mStreamToolbar`'s toggle: `UpdateStreamToolbarVisibility` is
    // the single source of truth for that bar (auto-shown when
    // streaming, idle otherwise) and a parallel menu toggle would
    // let the two states diverge.
    if (mMainToolbar != nullptr)
    {
        viewMenu->addAction(mMainToolbar->toggleViewAction());
    }

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
