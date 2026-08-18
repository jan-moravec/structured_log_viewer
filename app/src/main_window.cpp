#include "main_window.hpp"
#include "./ui_main_window.h"

#include "advanced_filter_editor.hpp"
#include "column_editor.hpp"
#include "columns_manager_dialog.hpp"
#include "configuration_diagnostics_dialog.hpp"
#include "export_dialog.hpp"
#include "export_sink.hpp"
#include "filter_editor.hpp"
#include "highlight_rule_set.hpp"
#include "highlight_rules_editor.hpp"
#include "histogram_model.hpp"
#include "icon_loader.hpp"
#include "leaf_rule_compile.hpp"
#include "level_cell_delegate.hpp"
#include "log_session.hpp"
#include "log_session_view.hpp"
#include "log_string_matcher.hpp"
#include "log_warning.hpp"
#include "network_stream_dialog.hpp"
#include "qstring_path.hpp"
#include "qt_streaming_log_sink.hpp"
#include "regex_template_registry.hpp"
#include "regex_templates_editor.hpp"
#include "row_exporter.hpp"
#include "session_bundle_dialog.hpp"
#include "session_history_manager.hpp"
#include "shortcuts_dialog.hpp"
#include "streaming_control.hpp"
#include "theme_control.hpp"
#include "uuid_utils.hpp"

#include <loglib/auto_detect_parser.hpp>
#include <loglib/bytes_producer.hpp>
#include <loglib/enum_dictionary.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/format_detection.hpp>
#include <loglib/internal/ascii_case.hpp>
#include <loglib/internal/decompressing_byte_source.hpp>
#include <loglib/internal/stdin_peek.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_factory.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_processing.hpp>
#include <loglib/parsers/csv_parser.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/parsers/logfmt_parser.hpp>
#include <loglib/parsers/regex_parser.hpp>
#include <loglib/regex_templates.hpp>
#include <loglib/rotation_siblings.hpp>
#include <loglib/session_bundle.hpp>
#include <loglib/stdin_bytes_producer.hpp>
#include <loglib/stop_token.hpp>
#include <loglib/stream_line_source.hpp>
#include <loglib/tailing_bytes_producer.hpp>
#include <loglib/tcp_server_producer.hpp>
#include <loglib/theme.hpp>
#include <loglib/udp_server_producer.hpp>

#include <QAbstractProxyModel>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCollator>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFont>
#include <QFontDatabase>
#include <QFuture>
#include <QFutureWatcher>
#include <QGuiApplication>
#include <QHeaderView>
#include <QInputDialog>
#include <QIntValidator>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointF>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStandardItemModel>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStringList>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QTableView>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>
#include <QVariant>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

namespace
{

// Check the extension without opening the bundle.
[[nodiscard]] bool IsSessionBundlePath(const QString &path)
{
    // `QFileInfo::suffix()` omits the leading dot.
    static_assert(
        loglib::SESSION_BUNDLE_EXTENSION[0] == '.' && loglib::SESSION_BUNDLE_EXTENSION[1] != '\0',
        "SESSION_BUNDLE_EXTENSION must be a dot-prefixed, non-empty extension"
    );
    return QFileInfo(path).suffix().compare(QLatin1String(loglib::SESSION_BUNDLE_EXTENSION + 1), Qt::CaseInsensitive) ==
           0;
}

// Detect filters that cannot bind to the current columns.
// NOLINTNEXTLINE(misc-no-recursion): mutually recursive with std::visit lambdas below.
[[nodiscard]] bool FilterHasUnresolvedLeaves(
    const loglib::FilterExpression &expression, const std::vector<loglib::LogConfiguration::Column> &columns
)
{
    return std::visit(
        // NOLINTBEGIN(misc-no-recursion): the visitor recurses back through FilterHasUnresolvedLeaves
        // for And/Or/Not; the recursion is structural and bounded by the parse-tree depth.
        [&columns]<class N>(const N &node) -> bool {
            if constexpr (std::is_same_v<N, loglib::FilterExpression::Leaf>)
            {
                return ResolveLeafColumnByKeys(node.rule.columnKeys, columns) < 0;
            }
            else if constexpr (
                std::is_same_v<N, loglib::FilterExpression::And> || std::is_same_v<N, loglib::FilterExpression::Or>
            )
            {
                return std::ranges::any_of(node.children, [&columns](const loglib::FilterExpression &child) {
                    return FilterHasUnresolvedLeaves(child, columns);
                });
            }
            else if constexpr (std::is_same_v<N, loglib::FilterExpression::Not>)
            {
                if (node.child == nullptr)
                {
                    return false;
                }
                return FilterHasUnresolvedLeaves(*node.child, columns);
            }
            else
            {
                return false;
            }
        },
        // NOLINTEND(misc-no-recursion)
        expression.node
    );
}

// Use relative recent-session times, with absolute dates after 30 days.
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

    // Preserve non-ASCII Windows paths; constructing a path from
    // `QString::toStdString()` would decode through the active code page.
    const auto appDir = logapp::QStringToFsPath(QCoreApplication::applicationDirPath());
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

// Poll cadence for the decompression progress dialog. 200 ms matches
// the existing streaming batch tick, keeps repaint cost low, and
// caps cancel latency.
constexpr int DECOMPRESSION_POLL_INTERVAL_MS = 200;

// `minimumDuration` for the decompression dialog -- decompressions
// completing under half a second never flash it.
constexpr int DECOMPRESSION_DIALOG_DEFER_MS = 500;

// Filtered-row export uses the same cadence + defer as
// decompression: small exports never flash the dialog, large
// exports update at ~5 Hz.
constexpr int EXPORT_POLL_INTERVAL_MS = 200;
constexpr int EXPORT_DIALOG_DEFER_MS = 500;

// Top of the `QProgressDialog` percent range.
constexpr int PROGRESS_PERCENT_MAX = 100;

// Format a byte count for the progress dialog / completion toast.
// Locale-independent on purpose: the temp path already pins the
// message to English, so a mixed-locale number would look worse
// than "MiB". Powers of 1024 (binary IEC) to match
// `std::filesystem::file_size` and Explorer's Size column.
//
// Use `%1` (not `%L1`) to keep `QString::arg(double, ..., 'f', ...)`
// formatting in the C locale.
QString HumanBytes(std::size_t bytes)
{
    constexpr std::array<const char *, 5> UNITS = {"B", "KiB", "MiB", "GiB", "TiB"};
    constexpr double BINARY_UNIT_SCALE = 1024.0;
    auto value = static_cast<double>(bytes);
    std::size_t unitIndex = 0;
    while (value >= BINARY_UNIT_SCALE && unitIndex + 1 < UNITS.size())
    {
        value /= BINARY_UNIT_SCALE;
        ++unitIndex;
    }
    if (unitIndex == 0)
    {
        return QStringLiteral("%1 %2").arg(bytes).arg(QString::fromLatin1(UNITS[0]));
    }
    return QStringLiteral("%1 %2").arg(value, 0, 'f', 1).arg(QString::fromLatin1(UNITS[unitIndex]));
}

// Format a wall-clock duration for the completion toast.
// Sub-second: "480 ms"; otherwise "X.Y s" or "Xm Ys" past a minute.
// C-locale-only, same reason as `HumanBytes`.
QString HumanDuration(std::chrono::steady_clock::duration d)
{
    constexpr std::int64_t MS_PER_SECOND = 1000;
    constexpr std::int64_t MS_PER_MINUTE = 60 * MS_PER_SECOND;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    if (ms < MS_PER_SECOND)
    {
        return QStringLiteral("%1 ms").arg(ms);
    }
    if (ms < MS_PER_MINUTE)
    {
        return QStringLiteral("%1 s").arg(static_cast<double>(ms) / static_cast<double>(MS_PER_SECOND), 0, 'f', 1);
    }
    const auto minutes = ms / MS_PER_MINUTE;
    const auto seconds = (ms % MS_PER_MINUTE) / MS_PER_SECOND;
    return QStringLiteral("%1m %2s").arg(minutes).arg(seconds);
}

// "Is this file a configuration?" classifier used by
// `DispatchMixedOpenInput`. Two-step: cheap structural peek
// (BOM-aware leading `{` and a `"columns"` substring in the first
// 4 KB) followed by a Glaze parse of the bounded prefix. Rejects
// `columns.empty()` so `{}` (a valid default `LogConfiguration`)
// is not misclassified as a configuration.
// Bound configuration sniffing so a log prefix cannot freeze the GUI.
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

// Decode persisted boolean values case-insensitively for hand-edited files.
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

// Categorize dropped filters for one load summary.
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
    // Empty when the leaf's column keys do not resolve.
    std::string columnHeader;
};

QString FilterValidationReasonString(FilterValidationReason reason)
{
    switch (reason)
    {
    case FilterValidationReason::OutOfRangeRow:
        return QStringLiteral("column keys did not resolve to any column");
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

// Validate saved leaves before load or editing; nullopt means valid.
std::optional<FilterValidationFailure> ValidateFilterAgainstColumns(
    const loglib::LeafRule &filter, const std::vector<loglib::LogConfiguration::Column> &columns
)
{
    using LeafType = loglib::LeafRule::Type;
    using ColumnType = loglib::LogConfiguration::Type;

    const int resolvedRow = ResolveLeafColumnByKeys(filter.columnKeys, columns);
    if (resolvedRow < 0)
    {
        return FilterValidationFailure{
            .reason = FilterValidationReason::OutOfRangeRow, .row = -1, .columnHeader = std::string{}
        };
    }

    const auto &column = columns[static_cast<size_t>(resolvedRow)];

    if (filter.type == LeafType::Enumeration && filter.filterValues.empty())
    {
        return FilterValidationFailure{
            .reason = FilterValidationReason::EmptyEnumSelection, .row = resolvedRow, .columnHeader = column.header
        };
    }

    const bool isNumericColumn =
        column.type == ColumnType::Integer || column.type == ColumnType::Floating || column.type == ColumnType::Number;
    const bool isEnumLikeColumn = column.type == ColumnType::Enumeration || column.type == ColumnType::Level;
    const bool typesMatch = (filter.type == LeafType::Time && column.type == ColumnType::Time) ||
                            (filter.type == LeafType::Enumeration && isEnumLikeColumn) ||
                            (filter.type == LeafType::Boolean && column.type == ColumnType::Boolean) ||
                            (filter.type == LeafType::Number && isNumericColumn) ||
                            (filter.type == LeafType::String && column.type != ColumnType::Time && !isEnumLikeColumn &&
                             column.type != ColumnType::Boolean && !isNumericColumn);
    if (!typesMatch)
    {
        return FilterValidationFailure{
            .reason = FilterValidationReason::TypeMismatch, .row = resolvedRow, .columnHeader = column.header
        };
    }

    switch (filter.type)
    {
    case LeafType::Time:
        // At least one bound must be set; `nullopt` on the other side is
        // fed to the predicate as INT64_MIN / INT64_MAX at construction.
        if (!filter.filterBegin.has_value() && !filter.filterEnd.has_value())
        {
            return FilterValidationFailure{
                .reason = FilterValidationReason::MissingTimeRange, .row = resolvedRow, .columnHeader = column.header
            };
        }
        break;
    case LeafType::Number:
        if (!filter.filterMinValue.has_value() && !filter.filterMaxValue.has_value())
        {
            return FilterValidationFailure{
                .reason = FilterValidationReason::MissingNumericRange, .row = resolvedRow, .columnHeader = column.header
            };
        }
        break;
    case LeafType::Boolean:
    {
        const BooleanFilterSides sides = DecodeBooleanFilterSides(filter.filterValues);
        if (!sides.includeTrue && !sides.includeFalse)
        {
            return FilterValidationFailure{
                .reason = FilterValidationReason::MissingBooleanSelection,
                .row = resolvedRow,
                .columnHeader = column.header
            };
        }
        break;
    }
    case LeafType::String:
        if (!filter.filterString.has_value() || !filter.matchType.has_value())
        {
            return FilterValidationFailure{
                .reason = FilterValidationReason::MissingStringMatch, .row = resolvedRow, .columnHeader = column.header
            };
        }
        break;
    case LeafType::Enumeration:
        // Empty-selection already handled above.
        break;
    }

    return std::nullopt;
}

// Extract stable column keys; out-of-range rows produce an inert empty key set.
[[nodiscard]] std::vector<std::string> ColumnKeysForRow(
    int rowIndex, const std::vector<loglib::LogConfiguration::Column> &columns
)
{
    if (rowIndex < 0 || static_cast<size_t>(rowIndex) >= columns.size())
    {
        return {};
    }
    return columns[static_cast<size_t>(rowIndex)].keys;
}

// Diagnostic for "no tzdata found" matching common.cpp's shape.
QString FormatTzdataNotFoundMessage(const std::vector<std::filesystem::path> &searched)
{
    QStringList lines;
    lines << QStringLiteral("Could not find the `tzdata/` directory required to initialize the timezone database.");
    lines << QStringLiteral("Searched the following candidate locations (in order):");
    for (const auto &p : searched)
    {
        lines << QStringLiteral("  - %1").arg(logapp::FsPathToQString(p));
    }
    lines << QString();
    lines << QStringLiteral(
        "Run the binary from a directory that has a sibling `tzdata/` "
        "(deployed installs ship one next to the executable; `cmake/FetchDependencies.cmake` "
        "stages it at `${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tzdata` for local builds)."
    );
    return lines.join(QLatin1Char('\n'));
}

// Shared format detection keeps file, stdin, and network opens
// consistent.
using loglib::DetectedFormat;
using loglib::DetectFormatForPath;
using loglib::MakeParserForFormat;

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : MainWindow(nullptr, nullptr, nullptr, parent)
{
}

MainWindow::MainWindow(ThemeControl *theme, QWidget *parent)
    : MainWindow(theme, nullptr, nullptr, parent)
{
}

MainWindow::MainWindow(
    ThemeControl *theme,
    SessionHistoryManager *historyManager,
    RegexTemplateRegistry *regexTemplateRegistry,
    QWidget *parent
)
    : QMainWindow(parent),
      ui(new Ui::MainWindow),
      mTheme(theme),
      mHistoryManager(historyManager),
      mRegexTemplateRegistry(regexTemplateRegistry),
      mOperations(std::make_unique<SessionOperationController>(*this))
{
#ifdef LOGAPP_BUILD_TESTING
    // Offscreen Qt cannot complete modal close or save dialogs.
    if (qEnvironmentVariable("QT_QPA_PLATFORM") == QLatin1String("offscreen"))
    {
        mSuppressDialogsForTest = true;
    }
#endif
    ui->setupUi(this);
    ApplyThemedWindowIcon();
    setAcceptDrops(true);

    // The active session owns the model objects; the shell keeps non-owning aliases.
    mSession = new LogSession(mTheme, mHistoryManager, mRegexTemplateRegistry, this);
    // Seed model aliases before constructing the view that supplies widget aliases.
    SetActiveSessionAliases(mSession, /*view=*/nullptr);

    // Title projection requires a constructed active session.
    UpdateWindowTitle();

    // Scoped subscriptions are installed after every referenced widget exists.

    // Preserve platform content margins while each tab page owns its zero-margin view layout.
    // `addTab` establishes Qt ownership for the initially unparented view.
    mSessionView = new LogSessionView(mSession, mTheme);
    mTabWidget = new QTabWidget(ui->centralWidget);
    mTabWidget->setTabsClosable(true);
    mTabWidget->setMovable(true);
    mTabWidget->setDocumentMode(true);
    mTabWidget->setUsesScrollButtons(true);
    mTabWidget->setElideMode(Qt::ElideRight);
    // QTabWidget exposes this container name; QTabBar exposes each tab's visible text.
    mTabWidget->setAccessibleName(tr("Session Tabs"));
    mLayout = new QVBoxLayout(ui->centralWidget);
    mLayout->addWidget(mTabWidget, 1);

    {
        // Adding the first page fires `currentChanged(0)` before
        // `mTabs` has an entry -- guard so `OnActiveTabChanged`
        // does not run against an inconsistent vector.
        const SuppressActiveTabChangeScope suppress(*this);
        mTabWidget->addTab(mSessionView, tr("Untitled"));
    }

    auto initialTab = std::make_unique<WindowTab>();
    initialTab->id = mSession->InstanceId();
    initialTab->session = mSession;
    initialTab->view = mSessionView;
    mTabs.push_back(std::move(initialTab));

    // Complete view aliases after the tab page exists.
    SetActiveSessionAliases(mSession, mSessionView);

    // Wire the tab strip signals. `currentChanged` drives the full
    // rebind sequence (`OnActiveTabChanged`); `tabCloseRequested`
    // routes the built-in close button through `CloseTabAtIndex`
    // so the last-tab-closes-window semantics stay centralised.
    // `tabMoved` on the underlying `QTabBar` keeps `mTabs` in
    // lockstep with the strip's index order.
    connect(mTabWidget, &QTabWidget::currentChanged, this, &MainWindow::OnActiveTabChanged);
    connect(mTabWidget, &QTabWidget::tabCloseRequested, this, &MainWindow::CloseTabAtIndex);
    connect(mTabWidget->tabBar(), &QTabBar::tabMoved, this, [this](int from, int to) {
        if (from < 0 || to < 0 || std::cmp_greater_equal(from, mTabs.size()) ||
            std::cmp_greater_equal(to, mTabs.size()))
        {
            return;
        }
        auto moved = std::move(mTabs[from]);
        mTabs.erase(mTabs.begin() + from);
        mTabs.insert(mTabs.begin() + to, std::move(moved));
    });

    // Background tabs still need dirty, chrome, and origin-bound completion updates.
    InstallPerTabPersistentConnections(mSession);

    // LogSessionView owns intrinsic table setup; the shell adds only shared UI wiring.

    // Shell-scoped columnsInserted work:
    //   * keep the highlight-editor's column picker in sync so a
    //     rule authored right after streaming discovers a new key
    //     sees it; and
    //   * recompile the filter tree when a leaf's resolution could
    //     actually change (column additions are append-only in the
    //     streaming path, so previously-resolved leaves stay
    //     resolved to the same column; the gate skips an otherwise-
    //     guaranteed `layoutChanged` rebuild on filter models whose
    //     leaves are all already bound — pinned by
    //     `TestEnumPromotedOnUnrelatedColumnDoesNotRebuildFilters`).
    //
    // Session-owned highlight binding keeps background models independent.
    // columnsInserted -> highlight editor + unresolved-leaf filter
    // check: moved into `InstallActiveSessionConnections()`.

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

    // Bottom docking avoids competing with the right-side detail stack.
    // Same lifecycle as the anchors dock: hidden by default, toggled
    // from View / toolbar / Ctrl+H.
    mHistogramDock = new HistogramDock(mModel, mTheme, mAnchors, this);
    addDockWidget(Qt::BottomDockWidgetArea, mHistogramDock);
    mHistogramDock->hide();
    connect(mHistogramDock, &HistogramDock::bucketClicked, this, &MainWindow::JumpToFirstRowInBucket);
    // Tick-strip clicks jump to the anchored row itself via
    // `SelectSourceRow`, not the bucket's first row (which may sit
    // next to the anchor but isn't the anchor).
    connect(mHistogramDock, &HistogramDock::anchorClicked, this, &MainWindow::SelectSourceRow);
    connect(mHistogramDock, &HistogramDock::timeRangeSelected, this, &MainWindow::AddTimeRangeFilterFromHistogram);

    mActionToggleHistogram = new QAction(tr("Histogram"), this);
    mActionToggleHistogram->setObjectName(QStringLiteral("actionToggleHistogram"));
    mActionToggleHistogram->setCheckable(true);
    mActionToggleHistogram->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_H));
    addAction(mActionToggleHistogram);
    WireDockToggle(mHistogramDock, mActionToggleHistogram, &HistogramDock::closed);
    // modelReset -> ApplyColumnVisibility: moved into
    // `InstallActiveSessionConnections()`.
    // Selection and rendering policy belong to each session view.

    // Single entry point for both Preferences-driven and
    // OS-driven theme refreshes. Skipped in the no-theme test
    // fixture path; theme-dependent assertions wire the themed
    // overload of `MainWindow`.
    if (mTheme != nullptr)
    {
        connect(mTheme, &ThemeControl::themeChanged, this, &MainWindow::OnThemeChanged);
    }

    ApplyTableStyleSheet();

    // Session and view construction guarantees a complete proxy and selection-model chain.
    //
    // `LogFilterModel::setSortRole` is intentionally never called:
    // it sorts via `loglib::CompareRows` straight against `LogTable`
    // (no `data(role)` round-trip), so the sort role no longer
    // drives behaviour. The deprecated no-op stays on the class for
    // one release for test and benchmark compatibility.

    // Overview rail wiring, view -> shell status/message routing,
    // progress strip cancel, and rail buckets-changed re-push: all
    // moved into `InstallActiveSessionConnections()`.

    mActionToggleOverviewRail = new QAction(tr("Overview rail"), this);
    mActionToggleOverviewRail->setObjectName(QStringLiteral("actionToggleOverviewRail"));
    mActionToggleOverviewRail->setCheckable(true);
    // `R` for Rail. `Ctrl+Shift+O` belongs to `actionOpenLogStream`
    // (see `main_window.ui`); binding it here as well produced a
    // Qt "ambiguous shortcut overload" warning at runtime.
    mActionToggleOverviewRail->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_R));
    addAction(mActionToggleOverviewRail);
    connect(mActionToggleOverviewRail, &QAction::toggled, this, &MainWindow::SetOverviewRailVisible);

    // Without a theme controller, retain Qt's standard text delegate.
    if (mTheme != nullptr)
    {
        mLevelCellDelegate = new LevelCellDelegate(mTheme, this);
        // Level-delegate reapply subscriptions moved into
        // `InstallActiveSessionConnections()`. The one-shot seed
        // stays inline so the delegate is attached now against the
        // freshly-constructed model.
        ApplyLevelCellDelegate();
    }

    mTableView->resizeColumnsToContents();

    // Apply shell-owned policies uniformly to the initial and later tab views.
    ApplyTableChromeToView(mSessionView);

    // Rebuild on demand. This is the only escape hatch when every
    // header section is hidden (right-click needs a visible section).
    connect(ui->menuView, &QMenu::aboutToShow, this, &MainWindow::RebuildViewMenu);

    connect(ui->actionNewSession, &QAction::triggered, this, &MainWindow::NewSession);
    connect(ui->actionNewWindow, &QAction::triggered, this, &MainWindow::NewWindow);
    // Disabled without a manager; `NewWindow` would no-op anyway,
    // but the menu state makes the affordance visible.
    ui->actionNewWindow->setEnabled(mHistoryManager != nullptr);
    connect(ui->actionOpen, &QAction::triggered, this, &MainWindow::OpenFiles);

    // Programmatic actions keep tab commands together without changing the UI form.
    //
    // Shortcuts:
    //   New Tab       Ctrl+T           (also QKeySequence::AddTab)
    //   Close Tab     Ctrl+W           (also QKeySequence::Close)
    //   Next Tab      Ctrl+Tab         (also QKeySequence::NextChild)
    //   Previous Tab  Ctrl+Shift+Tab   (also QKeySequence::PreviousChild)
    //   Open in Tab   (menu-only; Ctrl+Shift+T is already Follow tail)
    //
    // Window scope prevents identical shortcuts in peer windows from becoming ambiguous.
    // `addAction` registers the action on the window so
    // `QAction::shortcut()` works whether or not the action lands
    // in a menu bar.
    mActionNewTab = new QAction(tr("New &Tab"), this);
    mActionNewTab->setShortcut(QKeySequence(QStringLiteral("Ctrl+T")));
    mActionNewTab->setShortcutContext(Qt::WindowShortcut);
    mActionNewTab->setToolTip(tr("Open a new empty tab in this window."));
    connect(mActionNewTab, &QAction::triggered, this, [this]() { AddNewTab(/*makeActive=*/true); });
    addAction(mActionNewTab);

    mActionCloseTab = new QAction(tr("&Close Tab"), this);
    mActionCloseTab->setShortcut(QKeySequence(QStringLiteral("Ctrl+W")));
    mActionCloseTab->setShortcutContext(Qt::WindowShortcut);
    mActionCloseTab->setToolTip(tr("Close the current tab. Closing the last tab closes the window."));
    connect(mActionCloseTab, &QAction::triggered, this, [this]() {
        if (mTabWidget != nullptr)
        {
            CloseTabAtIndex(mTabWidget->currentIndex());
        }
    });
    addAction(mActionCloseTab);

    mActionNextTab = new QAction(tr("Next Tab"), this);
    // Mirror common browser-style tab navigation shortcuts.
    // Qt tolerates duplicate sequences on a single `QAction` (both
    // fire the same slot); the primary `Ctrl+Tab` binding stays
    // first so the shortcut sheet and menu render it, and the
    // page-navigation alias is offered as a secondary. No
    // platform-native collision on Windows / Linux / macOS -- the
    // sequences are unused by the surrounding widget hierarchy
    // once a `MainWindow` is focused.
    mActionNextTab->setShortcuts(
        {QKeySequence(QStringLiteral("Ctrl+Tab")), QKeySequence(QStringLiteral("Ctrl+PgDown"))}
    );
    mActionNextTab->setShortcutContext(Qt::WindowShortcut);
    connect(mActionNextTab, &QAction::triggered, this, [this]() {
        if (mTabWidget == nullptr || mTabWidget->count() < 2)
        {
            return;
        }
        const int next = (mTabWidget->currentIndex() + 1) % mTabWidget->count();
        mTabWidget->setCurrentIndex(next);
    });
    addAction(mActionNextTab);

    mActionPreviousTab = new QAction(tr("Previous Tab"), this);
    // Mirror common browser-style tab navigation shortcuts.
    mActionPreviousTab->setShortcuts(
        {QKeySequence(QStringLiteral("Ctrl+Shift+Tab")), QKeySequence(QStringLiteral("Ctrl+PgUp"))}
    );
    mActionPreviousTab->setShortcutContext(Qt::WindowShortcut);
    connect(mActionPreviousTab, &QAction::triggered, this, [this]() {
        if (mTabWidget == nullptr || mTabWidget->count() < 2)
        {
            return;
        }
        const int prev = (mTabWidget->currentIndex() + mTabWidget->count() - 1) % mTabWidget->count();
        mTabWidget->setCurrentIndex(prev);
    });
    addAction(mActionPreviousTab);

    mActionOpenInNewTab = new QAction(tr("Open in New Tab..."), this);
    // No shortcut because Ctrl+Shift+T already controls Follow tail.
    mActionOpenInNewTab->setToolTip(tr("Open one or more log files in a new tab, without touching the current tab."));
    connect(mActionOpenInNewTab, &QAction::triggered, this, [this]() {
        // Prompt first so cancellation does not leave an empty tab.
        const bool forceReplace = QGuiApplication::keyboardModifiers().testFlag(Qt::ShiftModifier);
        const QStringList files = QFileDialog::getOpenFileNames(
            this,
            tr("Select Log Files"),
            DefaultOpenDir(),
            tr("Structured Logs (*.json *.jsonl *.ndjson *.logfmt *.csv *.log *.txt "
               "*.json.gz *.jsonl.gz *.ndjson.gz *.logfmt.gz *.csv.gz *.log.gz *.txt.gz "
               "*.json.bz2 *.jsonl.bz2 *.ndjson.bz2 *.logfmt.bz2 *.csv.bz2 *.log.bz2 *.txt.bz2 "
               "*.json.xz *.jsonl.xz *.ndjson.xz *.logfmt.xz *.csv.xz *.log.xz *.txt.xz "
               "*.json.zst *.jsonl.zst *.ndjson.zst *.logfmt.zst *.csv.zst *.log.zst *.txt.zst "
               "*.gz *.bz2 *.xz *.zst);;All Files (*.*)")
        );
        if (files.isEmpty())
        {
            return;
        }
        RememberLastOpenDir(files.first());
        AddNewTab(/*makeActive=*/true);
        DispatchMixedOpenInput(files, forceReplace ? OpenMode::Replace : OpenMode::Append);
    });
    addAction(mActionOpenInNewTab);

    // Insert the tab actions on the File menu after `New Session`
    // so the menu order stays New Window / New Session / New Tab /
    // Open. Fallback to `insertAction` on the menu's first action
    // if `actionNewSession` was not found.
    if (ui->menuFile != nullptr && ui->actionOpen != nullptr)
    {
        ui->menuFile->insertAction(ui->actionOpen, mActionNewTab);
        ui->menuFile->insertAction(ui->actionOpen, mActionOpenInNewTab);
        ui->menuFile->insertSeparator(ui->actionOpen);
        // Close Tab lives before Exit so the destructive actions
        // stay grouped near the bottom of the File menu.
        if (ui->actionExit != nullptr)
        {
            ui->menuFile->insertAction(ui->actionExit, mActionCloseTab);
            ui->menuFile->insertSeparator(ui->actionExit);
        }
    }

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
    connect(ui->actionExportFilteredRows, &QAction::triggered, this, &MainWindow::ExportFilteredRows);
    connect(ui->actionExportSessionBundle, &QAction::triggered, this, &MainWindow::ExportSessionBundle);
    // File -> Exit quits the whole application. `closeAllWindows`
    // fires `closeEvent` on every top-level so each window's
    // auto-save flush runs; the default `quitOnLastWindowClosed`
    // then triggers the `aboutToQuit` fan. We deliberately don't
    // also call `QApplication::quit()` so a window that vetoes its
    // close (`event->ignore()`) keeps the app alive.
    connect(ui->actionExit, &QAction::triggered, this, [] { QApplication::closeAllWindows(); });

    // actionCopy -> mTableView bag connect moved into
    // `InstallActiveSessionConnections()` so a tab switch reinstalls
    // it against the newly-bound view's table.
    // Tooltip reflects `Find`'s smart toggle behaviour.
    ui->actionFind->setToolTip(tr("Find in logs. Press again to close."));
    connect(ui->actionFind, &QAction::triggered, this, &MainWindow::Find);

    // Goto Line / Goto Timestamp actions (Edit menu; `Ctrl+G`
    // and `Ctrl+Shift+G`).
    connect(ui->actionGotoLine, &QAction::triggered, this, &MainWindow::GotoLine);
    connect(ui->actionGotoTimestamp, &QAction::triggered, this, &MainWindow::GotoTimestamp);

    connect(ui->actionAddFilter, &QAction::triggered, this, [this]() { AddFilter(QUuid::createUuid().toString()); });
    connect(ui->actionAdvancedFilter, &QAction::triggered, this, &MainWindow::OpenAdvancedFilter);
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

    // Follow-tail scroll signals, actionFollowTail::toggled, and
    // jumpToTailRequested bag connects moved into
    // `InstallActiveSessionConnections()`.
    //
    // Seed from the action's initial checked state: the `.ui`
    // declares Follow on by default but `toggled` only fires on
    // changes, so without this one-shot the view would start
    // un-suppressed.
    if (mTableView != nullptr)
    {
        mTableView->SetPendingNewRowsSuppressed(ui->actionFollowTail->isChecked());
    }

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

    // Find-cache invalidation, rows-shown status, and sort status
    // bag connects moved into `InstallActiveSessionConnections()`.

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
        // Restore rail ticks from the surviving cache. Prefer
        // unbiased `bucketCounts` over the capped `sortedRows`
        // list; a naive push would paint only the first 10 000
        // hits after a common-needle reopen.
        PushFindMatchesToOverviewRail();
    });

    // Dropping the find bar clears the rail's match ticks (they
    // mirror the "*i* of *N*" indicator, which is only shown while
    // the bar is visible). `closed` covers X-button / Escape;
    // `visibilityChanged(false)` also covers tab inactivation in a
    // tabified dock group where `closed` doesn't fire. The cache
    // survives so a later reveal can restore without re-scanning.
    const auto clearOverviewRailMatchTicks = [this]() {
        if (mOverviewRailModel != nullptr)
        {
            mOverviewRailModel->SetMatchProxyRows({});
        }
    };
    connect(mFindDock, &FindDock::closed, this, clearOverviewRailMatchTicks);
    connect(mFindDock, &QDockWidget::visibilityChanged, this, [clearOverviewRailMatchTicks](bool visible) {
        if (!visible)
        {
            clearOverviewRailMatchTicks();
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
    mRecordDetailDock = new RecordDetailDock(mModel, mAnchors, this);
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

    // doubleClicked -> ShowRecordDetailsForProxyIndex and
    // RebindRecordDetailSelectionTracking() moved into
    // `InstallActiveSessionConnections()` (B2 fix — the tracking
    // rebind was previously only installed once at ctor time).

    // The dock owns its own `modelReset -> Clear` wiring, so reuse
    // outside `MainWindow` stays correct.

    mPreferencesEditor = new PreferencesEditor(mTheme, this);
    connect(ui->actionPreferences, &QAction::triggered, this, [this]() {
        mPreferencesEditor->UpdateFields();
        mPreferencesEditor->show();
        mPreferencesEditor->raise();
        mPreferencesEditor->activateWindow();
    });

    if (ui->menuSettings != nullptr)
    {
        mActionAutoDetectRotationHistory = new QAction(tr("Auto-detect rotated log history"), this);
        mActionAutoDetectRotationHistory->setObjectName(QStringLiteral("actionAutoDetectRotationHistory"));
        mActionAutoDetectRotationHistory->setCheckable(true);
        mActionAutoDetectRotationHistory->setToolTip(
            tr("When opening a log file, also load its rotated companions (`app.log.1`, "
               "`app.log-2025-04-28.gz`, ...) as the older prefix of the merged view.")
        );
        connect(mActionAutoDetectRotationHistory, &QAction::toggled, this, &MainWindow::OnRotationHistoryPrefToggled);
        ui->menuSettings->addSeparator();
        ui->menuSettings->addAction(mActionAutoDetectRotationHistory);

        mActionUndoRotationExpansion = new QAction(tr("Undo rotated history expansion"), this);
        mActionUndoRotationExpansion->setObjectName(QStringLiteral("actionUndoRotationExpansion"));
        mActionUndoRotationExpansion->setEnabled(false);
        mActionUndoRotationExpansion->setToolTip(
            tr("Revert to opening just the primary file; drops the auto-loaded rotated companions.")
        );
        connect(mActionUndoRotationExpansion, &QAction::triggered, this, &MainWindow::UndoRotationExpansion);
        ui->menuSettings->addAction(mActionUndoRotationExpansion);

        SyncRotationHistoryActionCheckedState();
    }

    // Settings -> Regex templates... opens the dedicated editor.
    // Built lazily so the widget tree only materialises on first
    // visit. Disabled without a registry (test fixtures / ad-hoc
    // instances) since the editor exists to mutate one.
    if (mRegexTemplateRegistry != nullptr)
    {
        connect(ui->actionRegexTemplates, &QAction::triggered, this, [this]() {
            if (mRegexTemplatesEditor == nullptr)
            {
                mRegexTemplatesEditor = new RegexTemplatesEditor(mRegexTemplateRegistry, this);
            }
            else
            {
                // Refresh on every menu open so out-of-band
                // registry changes (e.g. a Reload elsewhere) are
                // reflected in the list.
                mRegexTemplatesEditor->RefreshList();
            }
            mRegexTemplatesEditor->show();
            mRegexTemplatesEditor->raise();
            mRegexTemplatesEditor->activateWindow();
        });
    }
    else
    {
        ui->actionRegexTemplates->setEnabled(false);
        ui->actionRegexTemplates->setToolTip(
            tr("Regex templates editor needs a RegexTemplateRegistry (production-only).")
        );
    }

    // Settings -> Highlight rules... opens the modeless editor
    // (lazy-construct, survive-close, like the regex editor). Save
    // updates the runtime cache and the persistent mirror in one
    // atomic slot.
    connect(ui->actionHighlightRules, &QAction::triggered, this, &MainWindow::OpenHighlightRulesEditor);
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

    // Overview-rail width preset (live preview from Preferences).
    // Refresh the table's reserved margin after applying so the
    // viewport tracks the new sizeHint.
    connect(mPreferencesEditor, &PreferencesEditor::overviewRailWidthChanged, this, [this](OverviewRailWidthMode mode) {
        if (mOverviewRailWidget == nullptr)
        {
            return;
        }
        mOverviewRailWidget->SetWidthMode(mode);
        if (mTableView != nullptr)
        {
            mTableView->RefreshOverviewRailMargin();
        }
    });

    // Anchor hotkeys (programmatic so the .ui isn't bloated):
    //   Ctrl+1..8     anchor selection at colour N
    //   Ctrl+0        clear anchor on selection
    //   Ctrl+Shift+A  clear every anchor
    //   F2 / Shift+F2 jump to next / previous visible anchor
    //   F4            edit note on the current anchored row
    for (std::size_t i = 0; i < mAnchorColorActions.size(); ++i)
    {
        auto *action = new QAction(this);
        action->setText(tr("Anchor selection in colour %1").arg(i + 1));
        action->setShortcut(QKeySequence(Qt::CTRL | static_cast<Qt::Key>(Qt::Key_1 + static_cast<int>(i))));
        addAction(action);
        mAnchorColorActions[i] = action;
    }
    mActionClearRowAnchor = new QAction(tr("Remove anchor from selection"), this);
    mActionClearRowAnchor->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_0));
    addAction(mActionClearRowAnchor);
    // Anchor-color -> mTableView->AnchorSelection and
    // mActionClearRowAnchor -> mTableView->ClearAnchorOnSelection
    // bag connects moved into `InstallActiveSessionConnections()`
    // so a tab switch reinstalls them against the newly-bound view.

    mActionJumpNextAnchor = new QAction(tr("Jump to next anchor"), this);
    mActionJumpNextAnchor->setShortcut(QKeySequence(Qt::Key_F2));
    addAction(mActionJumpNextAnchor);
    connect(mActionJumpNextAnchor, &QAction::triggered, this, [this]() { JumpToAnchor(true); });

    mActionJumpPrevAnchor = new QAction(tr("Jump to previous anchor"), this);
    mActionJumpPrevAnchor->setShortcut(QKeySequence(Qt::SHIFT | Qt::Key_F2));
    addAction(mActionJumpPrevAnchor);
    connect(mActionJumpPrevAnchor, &QAction::triggered, this, [this]() { JumpToAnchor(false); });

    mActionEditRowAnchorNote = new QAction(tr("Edit anchor note on current row"), this);
    mActionEditRowAnchorNote->setObjectName(QStringLiteral("actionEditRowAnchorNote"));
    mActionEditRowAnchorNote->setShortcut(QKeySequence(Qt::Key_F4));
    addAction(mActionEditRowAnchorNote);
    connect(mActionEditRowAnchorNote, &QAction::triggered, this, &MainWindow::EditAnchorNoteOnCurrentRow);

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
    mStatusLabel->setObjectName(QStringLiteral("streamingStatusLabel"));
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
    // menus and this button stay in lock-
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
    // columnHealthChanged -> UpdateDiagnosticsStatus bag connect
    // moved into `InstallActiveSessionConnections()`.

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

    // Streaming lifecycle bag connects (lineCountChanged,
    // errorCountChanged, streamingFinished, rotationDetected,
    // rotationFlashChanged, sourceStatusChanged) and the enum-
    // column change -> filter rebuild pipeline moved into
    // `InstallActiveSessionConnections()`. The filter rebuild body
    // itself lives in `OnEnumColumnsChangedApplyFilterRebuild()`
    // so both the ctor install and the tab-switch reinstall share
    // one source of truth.

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

    // Seed the overview rail from the persisted preference (default
    // on). Routing through `SetOverviewRailVisible` keeps the
    // QAction, attach state, and settings write in one place. Width
    // mode is applied before attach so the first `ResolvedRailWidth`
    // sees the scaled sizeHint.
    if (mActionToggleOverviewRail != nullptr)
    {
        const QSettings settings;
        if (mOverviewRailWidget != nullptr)
        {
            mOverviewRailWidget->SetWidthMode(ParseOverviewRailWidthMode(
                settings.value(QStringLiteral("ui/overviewRailWidth"), QStringLiteral("medium")).toString()
            ));
        }
        const bool showOverviewRail = settings.value(QStringLiteral("ui/showOverviewRail"), true).toBool();
        SetOverviewRailVisible(showOverviewRail);
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

    // Install scoped subscriptions only after all receivers exist.
    InstallActiveSessionConnections();

    // Use the same ordered dock binding for construction and tab switches.
    RebindSharedDocks(activeSessionBindContext());

    // Timezone database initialisation lives in
    // `MainWindow::InitializeTimezoneDatabase`, called synchronously
    // from `main()` (and the QtTest fixture) before any window is
    // constructed. The constructor therefore stays free of
    // process-global side effects.
}

MainWindow::~MainWindow()
{
    // Disconnect scoped callbacks before any session teardown can emit.
    //
    // Direct bag Clear rather than routing through the
    // `UnbindActiveSessionForTest()` public entry: this is the
    // destructor's own teardown, not a test seam, and going
    // through a `ForTest` alias would read as accidental.
    mSessionConnections.Clear();
    // Per-tab callbacks must not outlive shell members they access.
    for (auto &tab : mTabs)
    {
        if (tab != nullptr)
        {
            tab->persistentConnections.Clear();
        }
    }

    // Defensive backstop in case any destruction path skipped
    // `closeEvent` (it normally runs first). Idempotent and cheap.
    DetachAutoSaveUuid();

    // Cancel + drain any in-flight decompression before its shared
    // state outlives us. The helper is a bounded blocking wait
    // (worker polls stop between 64 KiB chunks) and detaches the
    // future so the queued `finished` signal can't fire against a
    // half-destructed MainWindow.
    CancelInFlightDecompression();
    CancelInFlightExport();
    for (LogSession *session : hostedSessions())
    {
        if (session == nullptr || session == mSession)
        {
            continue;
        }
        CancelInFlightDecompressionFor(session);
        CancelInFlightExportFor(session);
    }

    if (mModel != nullptr)
    {
        const SessionSwitchScope destructorGuard(*this);
        mModel->Reset();
    }

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

// Active-session aliases and scoped subscriptions switch as one unit.

std::vector<LogSession *> MainWindow::hostedSessions() const
{
    // Walk `mTabs` so callers that need every hosted session
    // (window-modified aggregation, global-preference broadcast,
    // closeEvent PreCheckClose) see all tabs, not just the active
    // Fall back to the active alias during construction and narrow test setup.
    std::vector<LogSession *> sessions;
    if (!mTabs.empty())
    {
        sessions.reserve(mTabs.size());
        for (const auto &tab : mTabs)
        {
            if (tab != nullptr && tab->session != nullptr)
            {
                sessions.push_back(tab->session.data());
            }
        }
        return sessions;
    }
    if (mSession != nullptr)
    {
        sessions.push_back(mSession);
    }
    return sessions;
}

void MainWindow::SetActiveSessionAliases(LogSession *session, LogSessionView *view) noexcept
{
    Q_ASSERT_X(session != nullptr, "MainWindow::SetActiveSessionAliases", "session must not be null");
    if (session == nullptr)
    {
        return;
    }

    // Session-quintet aliases. Refresh unconditionally so callers
    // Refresh all model aliases together on every tab switch.
    mSession = session;
    mAnchors = session->Anchors();
    mHighlights = session->Highlights();
    mModel = session->Model();
    mRowOrderProxyModel = session->RowOrderProxy();
    mSortFilterProxyModel = session->FilterProxy();

    // View aliases. The ctor calls this once with view=nullptr
    // before `mSessionView` exists (so `mSession`-only reads work
    // between the two constructions), and once with view non-null
    // after `mSessionView` is constructed. Guard the view-side
    // aliases so the first call does not overwrite the view slots
    // with garbage.
    if (view != nullptr)
    {
        mSessionView = view;
        mTableView = view->TableView();
        mOverviewRailModel = view->OverviewRailModelPtr();
        mOverviewRailWidget = view->OverviewRail();
    }
}

void MainWindow::UnbindActiveSessionForTest() noexcept
{
    // Idempotent: `ScopedConnections::Clear` handles an already-empty
    // bag and disconnects only valid connections. Also called
    // Test teardown uses the same idempotent bag clear.
    mSessionConnections.Clear();
}

LogSession *MainWindow::LogSessionForDecompressionWatcher(const QObject *watcherSender) const
{
    // Match watcher ownership directly because decompressions may overlap across tabs.
    if (watcherSender == nullptr)
    {
        return nullptr;
    }
    for (LogSession *session : hostedSessions())
    {
        if (session == nullptr)
        {
            continue;
        }
        if (session->DecompressionWatcherPtr() == watcherSender)
        {
            return session;
        }
    }
    return nullptr;
}

LogSession *MainWindow::LogSessionForExportWatcher(const QObject *watcherSender) const
{
    // Symmetric with `LogSessionForDecompressionWatcher`. Currently
    // unused by `OnExportFinished` (which reads `sender()->parent()`
    // because export watchers are `new ExportWatcher(sessionOwner)`);
    // provided for tests and callers that need the same
    // hosted-sessions walk without relying on the parent link.
    if (watcherSender == nullptr)
    {
        return nullptr;
    }
    for (LogSession *session : hostedSessions())
    {
        if (session == nullptr)
        {
            continue;
        }
        if (session->ExportWatcherPtr() == watcherSender)
        {
            return session;
        }
    }
    return nullptr;
}

LogSessionView *MainWindow::LogSessionViewForSession(const LogSession *session) const
{
    if (session == nullptr)
    {
        return nullptr;
    }
    for (const auto &tab : mTabs)
    {
        if (tab != nullptr && tab->session.data() == session)
        {
            return tab->view.data();
        }
    }
    return nullptr;
}

SessionBindContext MainWindow::activeSessionBindContext() const
{
    // The factory maps missing teardown-time aliases to an unbound context.
    return SessionBindContext::FromSessionAndView(mSession, mSessionView, mTheme);
}

void MainWindow::CaptureHighlightRulesEditorDraft()
{
    if (mHighlightRulesEditor.isNull() || mHighlightRulesEditorSession.isNull())
    {
        return;
    }
    LogSession *origin = mHighlightRulesEditorSession.data();
    if (origin == nullptr)
    {
        return;
    }
    origin->SetHighlightEditorDraft(mHighlightRulesEditor->CaptureDraft());
}

void MainWindow::RestoreHighlightRulesEditorDraft(LogSession *session)
{
    if (mHighlightRulesEditor.isNull() || session == nullptr || session->Model() == nullptr)
    {
        mHighlightRulesEditorSession = session;
        return;
    }
    const auto &config = session->Model()->Configuration();
    mHighlightRulesEditor->SetColumns(config.columns);
    if (const std::optional<HighlightRulesEditorDraft> &draft = session->HighlightEditorDraft(); draft.has_value())
    {
        mHighlightRulesEditor->RestoreDraft(*draft);
    }
    else
    {
        HighlightRulesEditorDraft committed;
        committed.localRules = config.highlightRules;
        committed.baseline = config.highlightRules;
        committed.currentRow = config.highlightRules.empty() ? -1 : 0;
        mHighlightRulesEditor->RestoreDraft(committed);
    }
    mHighlightRulesEditorSession = session;
}

void MainWindow::OpenHighlightRulesEditor()
{
    if (mSession == nullptr || mModel == nullptr)
    {
        return;
    }
    if (mHighlightRulesEditor.isNull())
    {
        const auto &config = mModel->Configuration();
        mHighlightRulesEditor = new HighlightRulesEditor(config.highlightRules, config.columns, mTheme, this);
        mHighlightRulesEditor->setWindowFlag(Qt::Window, true);
        connect(
            mHighlightRulesEditor.data(),
            &HighlightRulesEditor::rulesSaved,
            this,
            [this](std::vector<loglib::LogConfiguration::HighlightRule> rules) {
                LogSession *origin = mHighlightRulesEditorSession.data();
                if (origin == nullptr || HostedSession(origin->InstanceId()) != origin || origin->Model() == nullptr)
                {
                    return;
                }
                auto forRuntime = rules;
                origin->Model()->ConfigurationManager().SetHighlightRules(std::move(rules));
                if (HighlightRuleSet *highlights = origin->Highlights(); highlights != nullptr)
                {
                    highlights->SetRules(
                        std::move(forRuntime), origin->Model()->Configuration().columns, &origin->Model()->Table()
                    );
                    const std::size_t inactive = highlights->InactiveCount();
                    if (inactive > 0)
                    {
                        statusBar()->showMessage(
                            tr("%1 highlight rule(s) inactive against current columns.")
                                .arg(static_cast<qulonglong>(inactive)),
                            STATUS_BAR_MESSAGE_TIMEOUT_MS
                        );
                    }
                }
                origin->MarkFiltersDirty();
                if (!mHighlightRulesEditor.isNull())
                {
                    origin->SetHighlightEditorDraft(mHighlightRulesEditor->CaptureDraft());
                }
            }
        );
        connect(mHighlightRulesEditor.data(), &HighlightRulesEditor::editsDiscarded, this, [this]() {
            if (LogSession *origin = mHighlightRulesEditorSession.data(); origin != nullptr)
            {
                origin->ClearHighlightEditorDraft();
            }
        });
    }
    RestoreHighlightRulesEditorDraft(mSession);
    mHighlightRulesEditor->show();
    mHighlightRulesEditor->raise();
    mHighlightRulesEditor->activateWindow();
}

void MainWindow::RebindSharedDocks(const SessionBindContext &context)
{
    // Bind state-owning docks before anchors, then reconcile session-scoped dialogs.

    // Preserve each session's parse-error log and presentation state.
    if (mParseErrorsDock != nullptr)
    {
        mParseErrorsDock->Bind(context);
    }

    // Cancel find debounce while swapping its session-owned query state.
    if (mFindDock != nullptr)
    {
        mFindDock->Bind(context);
    }

    // Restore histogram pin state after swapping guarded model pointers.
    if (mHistogramDock != nullptr)
    {
        mHistogramDock->Bind(context);
    }

    // Clear persistent indexes before swapping record-detail models.
    if (mRecordDetailDock != nullptr)
    {
        mRecordDetailDock->Bind(context);
    }

    // Rebuild anchor subscriptions and rows against the incoming session.
    if (mAnchorsDock != nullptr)
    {
        mAnchorsDock->Bind(context);
    }

    // Destroy dialogs whose captured model belongs to another session.
    auto destroyIfOriginMismatched = [&]<class T>(QPointer<T> &dialog, QPointer<LogSession> &origin) {
        if (dialog.isNull())
        {
            return;
        }
        if (origin.isNull() || origin.data() != context.session.data())
        {
            // `hide()` (not `close()`) so a dirty Highlight Rules
            // editor cannot pop a discard prompt if it is later
            // added to this path. Columns Manager and Diagnostics
            // have no discard modal, but the same hide + deleteLater
            // sequence lets queued callbacks observe a cleared
            // `QPointer` after this function returns.
            //
            // `deleteLater` reaps the QObject after the event loop
            // returns; the `QPointer` on the `m*Dialog` field auto-
            // nulls when the delete lands. `clear()` here nulls the
            // field synchronously so any code queued behind this
            // call still sees a null pointer and takes the
            // reconstruction path on the next open.
            dialog->hide();
            dialog->deleteLater();
            dialog.clear();
            origin.clear();
        }
    };
    destroyIfOriginMismatched(mDiagnosticsDialog, mDiagnosticsDialogSession);
    destroyIfOriginMismatched(mColumnsManagerDialog, mColumnsManagerDialogSession);

    if (!mHighlightRulesEditor.isNull())
    {
        LogSession *incoming = context.session.data();
        if (mHighlightRulesEditorSession.data() != incoming)
        {
            CaptureHighlightRulesEditorDraft();
            RestoreHighlightRulesEditorDraft(incoming);
        }
        else if (incoming != nullptr)
        {
            mHighlightRulesEditor->SetColumns(incoming->Model()->Configuration().columns);
        }
    }
}

// Tab lifecycle.

int MainWindow::TabCount() const noexcept
{
    return mTabWidget != nullptr ? mTabWidget->count() : 0;
}

int MainWindow::ActiveTabIndex() const noexcept
{
    return mTabWidget != nullptr ? mTabWidget->currentIndex() : -1;
}

int MainWindow::TabIndexForSession(SessionInstanceId id) const noexcept
{
    if (!id.isValid())
    {
        return -1;
    }
    for (int index = 0; std::cmp_less(index, mTabs.size()); ++index)
    {
        if (mTabs[index] != nullptr && mTabs[index]->id == id)
        {
            return index;
        }
    }
    return -1;
}

LogSession *MainWindow::HostedSession(SessionInstanceId id) const noexcept
{
    return SessionAtTab(TabIndexForSession(id));
}

std::size_t MainWindow::PerTabConnectionCountForTest(int index) const noexcept
{
    if (index < 0 || std::cmp_greater_equal(index, mTabs.size()) || mTabs[index] == nullptr)
    {
        return 0;
    }
    return mTabs[index]->persistentConnections.Size();
}

LogSession *MainWindow::SessionAtTab(int index) const noexcept
{
    if (index < 0 || std::cmp_greater_equal(index, mTabs.size()) || mTabs[index] == nullptr)
    {
        return nullptr;
    }
    return mTabs[index]->session.data();
}

LogSessionView *MainWindow::ViewAtTab(int index) const noexcept
{
    if (index < 0 || std::cmp_greater_equal(index, mTabs.size()) || mTabs[index] == nullptr)
    {
        return nullptr;
    }
    return mTabs[index]->view.data();
}

SessionInstanceId MainWindow::AddNewTab(bool makeActive)
{
    if (mTabWidget == nullptr)
    {
        return {};
    }

    // Fresh session parented on `this` (MainWindow) so its lifetime
    // tracks the window's, not any specific tab's view widget. Same
    // shape as the ctor's initial `mSession` construction. The view
    // is parented to nullptr because `QTabWidget::addTab` reparents
    // it onto the strip's internal stacked widget in one step.
    auto *session = new LogSession(mTheme, mHistoryManager, mRegexTemplateRegistry, this);
    auto *view = new LogSessionView(session, mTheme);

    // Shell-owned table chrome parity with the initial tab (H4 fix).
    // Also runs the theme stylesheet + level-icon delegate seed so
    // the freshly-constructed view matches the ctor-time appearance.
    ApplyTableChromeToView(view);

    auto record = std::make_unique<WindowTab>();
    record->id = session->InstanceId();
    record->session = session;
    record->view = view;
    mTabs.push_back(std::move(record));

    int newIndex = -1;
    {
        // The `addTab` call fires `currentChanged` iff this becomes
        // the new current index (typically only for the first tab
        // in an empty strip). Guard it so `OnActiveTabChanged` does
        // not attempt a mid-construction rebind: we handle the
        // activation explicitly via `setCurrentIndex` below when
        // `makeActive` is true.
        const SuppressActiveTabChangeScope suppress(*this);
        newIndex = mTabWidget->addTab(view, tr("Untitled"));
    }

    InstallPerTabPersistentConnections(session);
    RefreshTabChrome(session);

    if (makeActive && newIndex >= 0)
    {
        mTabWidget->setCurrentIndex(newIndex);
    }
    return session->InstanceId();
}

void MainWindow::InstallPerTabPersistentConnections(LogSession *session)
{
    if (session == nullptr)
    {
        return;
    }
    WindowTab *tab = nullptr;
    for (auto &record : mTabs)
    {
        if (record != nullptr && record->session.data() == session)
        {
            tab = record.get();
            break;
        }
    }
    if (tab == nullptr)
    {
        return;
    }

    const SessionInstanceId id = session->InstanceId();
    // These callbacks must fire for background tabs and disconnect before the
    // tab leaves the hosted registry.
    tab->persistentConnections += connect(session, &LogSession::presentationChanged, this, [this, id]() {
        if (LogSession *hosted = HostedSession(id); hosted != nullptr)
        {
            RefreshTabChrome(hosted);
        }
    });
    tab->persistentConnections +=
        connect(session, &LogSession::filtersDirtyChanged, this, [this](bool /*dirty*/) { UpdateWindowTitle(); });
    if (const LogModel *model = session->Model(); model != nullptr)
    {
        tab->persistentConnections +=
            connect(model, &LogModel::streamingFinished, this, [this, id](StreamingResult result) {
                HandleStreamingFinishedFor(HostedSession(id), result);
            });
    }
    if (LogSessionView *view = tab->view.data(); view != nullptr)
    {
        tab->persistentConnections += connect(view, &LogSessionView::progressCancelRequested, this, [this, id]() {
            LogSession *hosted = HostedSession(id);
            if (hosted == nullptr)
            {
                return;
            }
            if (hosted->IsDecompressionInFlight())
            {
                hosted->MutableDecompressionStopSource().request_stop();
            }
            if (hosted->IsExportInFlight())
            {
                hosted->MutableExportStopSource().request_stop();
            }
        });
    }
}

void MainWindow::HandleStreamingFinishedFor(LogSession *origin, StreamingResult result)
{
    if (mOperations == nullptr)
    {
        return;
    }
    mOperations->CompleteStreaming(origin, result);
}

void MainWindow::ApplyTableChromeToView(LogSessionView *view)
{
    if (view == nullptr)
    {
        return;
    }
    LogTableView *table = view->TableView();
    if (table == nullptr)
    {
        return;
    }
    // Apply identical scrolling behavior to every tab.
    table->setShowGrid(true);
    table->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    table->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    // Context-menu policy: required for the shell's header +
    // row context menus to open at all (`ShowHeaderContextMenu`
    // / `ShowRowContextMenu` are wired via the scoped bag against
    // the ACTIVE tab; without the policy they never fire).
    table->setContextMenuPolicy(Qt::CustomContextMenu);
    if (QHeaderView *header = table->horizontalHeader(); header != nullptr)
    {
        header->setContextMenuPolicy(Qt::CustomContextMenu);
    }
    // Level cell delegate: attach now so new tabs render the
    // level-icon column correctly from the first paint. The
    // scoped bag reapplies on `modelReset` / `columnsInserted` /
    // `columnsRemoved` / `enumColumnsChanged` while this view is
    // active. Skipped in the no-theme test fixture where
    // `mLevelCellDelegate` is null.
    if (mLevelCellDelegate != nullptr)
    {
        view->ApplyLevelCellDelegate(mLevelCellDelegate);
    }
}

void MainWindow::RebuildFilterMenuFromActiveSession()
{
    // Rebuild only the menu mirror; tab switching must not mutate filter state.
    if (ui == nullptr || ui->menuFilters == nullptr)
    {
        return;
    }
    // Drop menu entries tagged with a filter UUID (they belong to
    // whichever session was last active).
    const auto priorActions = ui->menuFilters->actions();
    for (QAction *action : priorActions)
    {
        if (action == nullptr)
        {
            continue;
        }
        if (!action->data().toString().isNull())
        {
            ui->menuFilters->removeAction(action);
            delete action;
        }
    }
    if (mSession == nullptr)
    {
        return;
    }
    // Rebuild in display order so mirror parity is preserved. The
    // wiring below mirrors the tail of `AddLogFilter` (menu +
    // Edit / Remove sub-actions) but does NOT call the session-
    // mutating first half.
    for (const std::string &idKey : mSession->SimpleLeafOrder())
    {
        const auto it = mSession->SimpleLeaves().find(idKey);
        if (it == mSession->SimpleLeaves().end())
        {
            continue;
        }
        const QString id = QString::fromStdString(idKey);
        const loglib::LeafRule &filter = it->second;
        const QString title = BuildFilterTitle(filter);
        QMenu *menuItem = ui->menuFilters->addMenu(title);
        menuItem->setObjectName(id);
        menuItem->menuAction()->setData(QVariant(id));
        const QAction *editAction = menuItem->addAction(tr("Edit"));
        connect(editAction, &QAction::triggered, this, [this, id]() {
            const auto found = Filters().find(id.toStdString());
            if (found == Filters().end())
            {
                AddFilter(id);
                return;
            }
            AddFilter(id, found->second);
        });
        const QAction *removeAction = menuItem->addAction(tr("Remove"));
        connect(removeAction, &QAction::triggered, this, [this, id]() { ClearFilter(id); });
    }
}

void MainWindow::ApplyPendingPresentation(LogSession *session)
{
    if (session == nullptr)
    {
        return;
    }
    SessionPendingPresentation pending = session->TakePendingPresentation();
    RefreshTabChrome(session);
    if (!pending.statusMessage.isEmpty())
    {
        statusBar()->showMessage(pending.statusMessage, pending.statusTimeoutMs);
    }
    if (pending.raiseParseErrors && mParseErrorsDock != nullptr)
    {
        if (!mParseErrorsDock->isVisible())
        {
            mParseErrorsDock->show();
        }
        mParseErrorsDock->raise();
    }
    if (pending.failureMessage.isEmpty())
    {
        return;
    }
#ifdef LOGAPP_BUILD_TESTING
    if (mSuppressDialogsForTest)
    {
        return;
    }
#endif
    QMessageBox::warning(this, pending.failureTitle, pending.failureMessage);
}

SessionInstanceId MainWindow::AddNewTabForTest(bool makeActive)
{
    return AddNewTab(makeActive);
}

void MainWindow::ActivateTabForTest(int index)
{
    if (mTabWidget != nullptr && index >= 0 && index < mTabWidget->count())
    {
        mTabWidget->setCurrentIndex(index);
    }
}

void MainWindow::CloseTabForTest(int index)
{
    CloseTabAtIndex(index);
}

void MainWindow::OpenHighlightRulesEditorForTest()
{
    OpenHighlightRulesEditor();
}

HighlightRulesEditor *MainWindow::HighlightRulesEditorForTest() const noexcept
{
    return mHighlightRulesEditor.data();
}

ColumnsManagerDialog *MainWindow::ColumnsManagerDialogForTest() const noexcept
{
    return mColumnsManagerDialog.data();
}

void MainWindow::ShowConfigurationDiagnosticsForTest()
{
    ShowConfigurationDiagnostics();
}

ConfigurationDiagnosticsDialog *MainWindow::ConfigurationDiagnosticsDialogForTest() const noexcept
{
    return mDiagnosticsDialog.data();
}

void MainWindow::CloseTabAtIndex(int index)
{
    if (mTabWidget == nullptr || index < 0 || std::cmp_greater_equal(index, mTabs.size()))
    {
        return;
    }

    // Route the final tab through window close so normal save and drain logic runs.
    if (mTabWidget->count() <= 1)
    {
        close();
        return;
    }

    // Preserve the active tab. Close-decision autosave uses origin
    // aliases and does not require activating a background tab.
    LogSession *closingSessionMutable = SessionAtTab(index);
    if (!PrepareSessionClose(closingSessionMutable))
    {
        return;
    }
    if (closingSessionMutable != nullptr)
    {
        // Preserve until close-decision save or Discard has completed.
        closingSessionMutable->DetachAutoSaveUuid();
    }

    // Preserve index validity while ownership moves out of the tab record:
    //   (1) disconnect that tab's persistent connections,
    //   (2) drop it from the hosted registry,
    //   (3) `removeTab` (fires `currentChanged(newIndex)` if the
    //       removed tab was current; `OnActiveTabChanged` rebinds
    //       the surviving session and view together),
    //   (4) cancel and drain workers on the unhosted session so
    //       queued `finished` slots resolve as unhosted and cannot
    //       start the next file or write into a sibling,
    //   (5) `deleteLater` both owned QObjects so any remaining
    //       callbacks unwind on the event loop rather than mid-
    //       destructor.
    LogSession *session = mTabs[index] ? mTabs[index]->session.data() : nullptr;
    LogSessionView *view = mTabs[index] ? mTabs[index]->view.data() : nullptr;
    if (mTabs[index] != nullptr)
    {
        mTabs[index]->persistentConnections.Clear();
    }
    if (session != nullptr)
    {
        session->MutablePendingOpenFiles().clear();
        if (auto *watcher = session->DecompressionWatcherPtr(); watcher != nullptr)
        {
            disconnect(
                watcher, &LogSession::DecompressionWatcher::finished, this, &MainWindow::OnDecompressionFinished
            );
        }
        if (auto *watcher = session->ExportWatcherPtr(); watcher != nullptr)
        {
            disconnect(watcher, &LogSession::ExportWatcher::finished, this, &MainWindow::OnExportFinished);
        }
    }

    mTabs.erase(mTabs.begin() + index);
    mTabWidget->removeTab(index);

    if (session != nullptr)
    {
        CancelInFlightDecompressionFor(session);
        CancelInFlightExportFor(session);
        if (LogModel *model = session->Model(); model != nullptr)
        {
            model->Reset();
        }
    }

    if (view != nullptr)
    {
        view->setParent(nullptr);
        view->deleteLater();
    }
    if (session != nullptr)
    {
        session->deleteLater();
    }
}

void MainWindow::OnActiveTabChanged(int newIndex)
{
    if (mSuppressActiveTabChange)
    {
        return;
    }
    if (mTabWidget == nullptr || newIndex < 0 || std::cmp_greater_equal(newIndex, mTabs.size()))
    {
        return;
    }
    auto &record = mTabs[newIndex];
    if (record == nullptr)
    {
        return;
    }
    LogSession *newSession = record->session.data();
    LogSessionView *newView = record->view.data();
    if (newSession == nullptr || newView == nullptr)
    {
        return;
    }

    // Reordering can report the current session again; avoid a redundant rebind.
    if (newSession == mSession)
    {
        return;
    }

    // (1) Save outgoing tab's focus widget so a later re-activate
    // restores what the user was on. Best-effort: focus may live
    // in a dock or dialog (application focus), which we do not
    // attempt to restore per-tab.
    const LogSession *outgoingSession = mSession;
    for (auto &tab : mTabs)
    {
        if (tab != nullptr && tab->session.data() == outgoingSession)
        {
            QWidget *focused = QApplication::focusWidget();
            if (focused != nullptr && tab->view != nullptr && tab->view->isAncestorOf(focused))
            {
                tab->lastFocus = focused;
            }
            break;
        }
    }

    // (2) Re-point aliases at the new tab's session + view.
    // Shared docks bind directly to the incoming context; each Bind
    // saves outgoing state. Skipping an unbound intermediate keeps
    // hidden docks from walking the outgoing model.
    mSessionConnections.Clear();
    SetActiveSessionAliases(newSession, newView);
    InstallActiveSessionConnections();
    RebindSharedDocks(activeSessionBindContext());

    // (3) Refresh shell-owned UI mirrors from the new active session.
    UpdateWindowTitle();
    UpdateStreamingStatus();
    UpdateRowsShownStatus();
    UpdateSortStatus();
    UpdateDiagnosticsStatus();
    UpdateStreamToolbarVisibility();
    RebuildFilterMenuFromActiveSession();
    SyncClearAllFiltersEnabled();
    SyncRotationHistoryActionCheckedState();
    if (mActionUndoRotationExpansion != nullptr && mSession != nullptr)
    {
        mActionUndoRotationExpansion->setEnabled(!mSession->LastRotationExpansionOriginalInputs().isEmpty());
    }
    ApplyColumnVisibility();
    if (mLevelCellDelegate != nullptr && newView != nullptr)
    {
        newView->ApplyLevelCellDelegate(mLevelCellDelegate);
    }
    // Reseed the follow-tail suppression flag from the toolbar
    // action's current checked state so the new view's pill
    // suppression matches the shell action's cache.
    if (ui != nullptr && ui->actionFollowTail != nullptr && mTableView != nullptr)
    {
        mTableView->SetPendingNewRowsSuppressed(ui->actionFollowTail->isChecked());
    }

    // (4) Restore focus. Prefer the tab's last-known focus widget;
    // fall back to the table view so keyboard nav is immediate.
    QWidget *focusTarget = record->lastFocus.data();
    if (focusTarget == nullptr && record->view != nullptr)
    {
        focusTarget = record->view->TableView();
    }
    if (focusTarget != nullptr)
    {
        focusTarget->setFocus(Qt::OtherFocusReason);
    }

    ApplyPendingPresentation(newSession);
}

void MainWindow::RefreshTabChrome(const LogSession *session)
{
    if (mTabWidget == nullptr || session == nullptr)
    {
        return;
    }
    const int index = TabIndexForSession(session->InstanceId());
    if (index < 0)
    {
        return;
    }

    // Derive tab chrome from the owning session, including background state.
    const SessionPresentationSnapshot snapshot = session->PresentationSnapshot();

    QString label = snapshot.shortLabel;
    if (label.isEmpty())
    {
        label = tr("Untitled");
    }

    // Non-color-only indicators keep status visible without colour.
    // Operation words are part of the tab text so assistive
    // technology can announce them; glyphs may prefix the label.
    const std::uint32_t ops = snapshot.operations;
    QString prefix;
    QString operationWord;
    if ((ops & static_cast<std::uint32_t>(SessionOperationState::Failed)) != 0)
    {
        prefix = QStringLiteral("\u26A0 "); // Warning: queued failure.
        operationWord = tr("Failed");
    }
    else if ((ops & static_cast<std::uint32_t>(SessionOperationState::Disconnected)) != 0)
    {
        prefix = QStringLiteral("\u2205 "); // Empty set: disconnected placeholder.
        operationWord = tr("Disconnected");
    }
    else if ((ops & static_cast<std::uint32_t>(SessionOperationState::Paused)) != 0)
    {
        prefix = QStringLiteral("\u23F8 "); // Pause: user paused live-tail.
        operationWord = tr("Paused");
    }
    else if ((ops & static_cast<std::uint32_t>(SessionOperationState::Ingesting)) != 0)
    {
        prefix = QStringLiteral("\u25B6 "); // Play triangle: live-tail / network producer active.
        operationWord = tr("Ingesting");
    }
    else if ((ops & static_cast<std::uint32_t>(SessionOperationState::Decompressing)) != 0)
    {
        prefix = QStringLiteral("\u21BB "); // Circular arrow: decompressing.
        operationWord = tr("Decompressing");
    }
    else if ((ops & static_cast<std::uint32_t>(SessionOperationState::Exporting)) != 0)
    {
        prefix = QStringLiteral("\u21E7 "); // Upwards arrow: exporting.
        operationWord = tr("Exporting");
    }
    else if ((ops & static_cast<std::uint32_t>(SessionOperationState::Parsing)) != 0)
    {
        prefix = QStringLiteral("\u2026 "); // Ellipsis: parsing static open.
        operationWord = tr("Parsing");
    }
    else if ((ops & static_cast<std::uint32_t>(SessionOperationState::SourceWaiting)) != 0)
    {
        prefix = QStringLiteral("\u29D6 "); // Hourglass-ish: source waiting.
        operationWord = tr("Waiting");
    }

    QString suffix;
    if (snapshot.dirty.filtersDirty)
    {
        suffix = QStringLiteral(" \u25CF"); // Bullet: unsaved filter/sort/column changes.
    }
    if (snapshot.errorCount > 0)
    {
        // Trailing exclamation for parse errors so screen readers
        // announce it even without the tooltip.
        suffix += QStringLiteral(" !");
    }

    QString tabText = prefix + label;
    if (!operationWord.isEmpty())
    {
        tabText += QStringLiteral(" \u2014 ");
        tabText += operationWord;
    }
    tabText += suffix;
    mTabWidget->setTabText(index, tabText);

    QString tooltip = snapshot.tooltip;
    if (tooltip.isEmpty())
    {
        tooltip = label;
    }
    tooltip += QLatin1Char('\n');
    tooltip += snapshot.statusSummary.isEmpty() ? tr("Idle") : snapshot.statusSummary;
    if (snapshot.dirty.filtersDirty)
    {
        tooltip += QLatin1Char('\n');
        tooltip += tr("Unsaved changes");
    }
    if (snapshot.errorCount > 0)
    {
        tooltip += QLatin1Char('\n');
        tooltip += tr("%n parse error(s)", nullptr, static_cast<int>(snapshot.errorCount));
    }
    mTabWidget->setTabToolTip(index, tooltip);

    // QTabBar exposes visible tab text to accessibility; Qt has no per-tab name setter.
}

void MainWindow::InstallActiveSessionConnections()
{
    // Construction and tab switching install the same scoped connection set.
    //
    // Contract:
    //   * every connect below is prefixed with
    //     `mSessionConnections +=` so `Clear()` reaps the whole
    //     set atomically on tab switch;
    //   * slots that read shell aliases (`mModel`, `mTableView`,
    //     ...) are safe because `SetActiveSessionAliases` has
    //     already re-pointed them before we reinstall;
    //   * connects whose LAMBDA captures a session pointer
    //     directly (rare -- see the enum-column pipeline) use
    //     the member function `OnEnumColumnsChangedApplyFilterRebuild`
    //     so there is only one place the body lives.
    //
    // NOT installed here (kept as persistent per-tab connects in
    // the ctor and `AddNewTab`):
    //   * `LogSession::presentationChanged -> RefreshTabChrome`
    //     (H3): a background tab must keep refreshing its own
    //     tab-strip chrome even while it is not active.
    //   * `LogSession::filtersDirtyChanged -> UpdateWindowTitle`
    //     (H2): a background edit must still light the window's
    //     `[*]` marker via the aggregate.
    //   * shared-dock `Bind` / `Unbind`: those live on the docks
    //     and are re-driven by `RebindSharedDocks` after this
    //     helper runs.
    //   * `LogSessionView::progressCancelRequested`: stored on the tab so a
    //     background tab's progress strip can cancel that session's workers.
    if (mSession == nullptr || mModel == nullptr || mSessionView == nullptr || mTableView == nullptr ||
        mSortFilterProxyModel == nullptr || mOverviewRailModel == nullptr || mOverviewRailWidget == nullptr)
    {
        return;
    }

    // --- Session-owned signals ------------------------------------------
    // rotationFlashChanged only meaningfully drives the ACTIVE tab's
    // status bar, so it stays bag-scoped. `filtersDirtyChanged` and
    // `presentationChanged` are wired PERSISTENTLY per-tab (in the
    // ctor for the initial tab and in `AddNewTab` for later tabs)
    // so background tabs still feed the window's aggregate dirty
    // marker and refresh their own tab chrome.
    mSessionConnections += connect(mSession, &LogSession::rotationFlashChanged, this, [this](bool /*active*/) {
        UpdateStreamingStatus();
    });

    // --- Model-owned signals: columns/level delegate/highlight sync ----
    mSessionConnections +=
        connect(mModel, &QAbstractItemModel::columnsInserted, this, [this](const QModelIndex &, int, int) {
            if (mHighlights == nullptr || mModel == nullptr)
            {
                return;
            }
            if (mHighlightRulesEditor != nullptr && mHighlightRulesEditorSession.data() == mSession)
            {
                mHighlightRulesEditor->SetColumns(mModel->Configuration().columns);
            }
            if (FilterHasUnresolvedLeaves(mModel->Configuration().expression, mModel->Configuration().columns))
            {
                UpdateFilters();
            }
        });
    mSessionConnections += connect(mModel, &QAbstractItemModel::modelReset, this, &MainWindow::ApplyColumnVisibility);
    if (mTheme != nullptr)
    {
        mSessionConnections +=
            connect(mModel, &QAbstractItemModel::modelReset, this, &MainWindow::ApplyLevelCellDelegate);
        mSessionConnections +=
            connect(mModel, &QAbstractItemModel::columnsInserted, this, &MainWindow::ApplyLevelCellDelegate);
        mSessionConnections +=
            connect(mModel, &QAbstractItemModel::columnsRemoved, this, &MainWindow::ApplyLevelCellDelegate);
        mSessionConnections += connect(
            mModel, &LogModel::enumColumnsChanged, this, [this](EnumColumnsChangeReason reason, int /*columnIndex*/) {
                if (reason == EnumColumnsChangeReason::Grew)
                {
                    return;
                }
                ApplyLevelCellDelegate();
            }
        );
    }

    // --- View / rail / progress ----------------------------------------
    mSessionConnections +=
        connect(mOverviewRailWidget, &OverviewRailWidget::proxyRowClicked, this, &MainWindow::ScrollToProxyRow);
    mSessionConnections += connect(mSessionView, &LogSessionView::rowNotVisible, this, [this]() {
        statusBar()->showMessage(tr("Row is not currently visible."), STATUS_BAR_MESSAGE_TIMEOUT_MS);
    });
    mSessionConnections += connect(mSessionView, &LogSessionView::followTailDisengageRequested, this, [this]() {
        if (ui != nullptr && ui->actionFollowTail != nullptr && ui->actionFollowTail->isChecked())
        {
            ui->actionFollowTail->setChecked(false);
        }
    });
    mSessionConnections +=
        connect(mSessionView, &LogSessionView::statusMessageRequested, this, [this](const QString &message) {
            statusBar()->showMessage(message, STATUS_BAR_MESSAGE_TIMEOUT_MS);
        });
    mSessionConnections += connect(mOverviewRailModel, &OverviewRailModel::bucketsChanged, this, [this]() {
        const auto &findCacheOpt = mSession->FindMatchCacheState();
        if (!IsFindBarVisible() || !findCacheOpt.has_value() || mOverviewRailModel == nullptr)
        {
            return;
        }
        const std::size_t n = mOverviewRailModel->BucketCount();
        if (n == 0)
        {
            return;
        }
        if (findCacheOpt->bucketCounts.size() == n && mOverviewRailModel->HasMatchTicks())
        {
            return;
        }
        PushFindMatchesToOverviewRail();
    });

    // --- Header / row context menus + column moves ---------------------
    mSessionConnections +=
        connect(mTableView->horizontalHeader(), &QHeaderView::sectionMoved, this, &MainWindow::OnHeaderSectionMoved);
    mSessionConnections += connect(
        mTableView->horizontalHeader(),
        &QHeaderView::customContextMenuRequested,
        this,
        &MainWindow::ShowHeaderContextMenu
    );
    mSessionConnections +=
        connect(mTableView, &QWidget::customContextMenuRequested, this, &MainWindow::ShowRowContextMenu);
    mSessionConnections += connect(mModel, &QAbstractItemModel::columnsMoved, this, &MainWindow::OnSourceColumnsMoved);
    mSessionConnections += connect(
        mModel, &LogModel::enumColumnsChanged, this, [this](EnumColumnsChangeReason /*reason*/, int /*columnIndex*/) {
            if (mHighlights == nullptr || mModel == nullptr)
            {
                return;
            }
            mHighlights->RebindColumns(mModel->Configuration().columns, &mModel->Table());
        }
    );
    // Enum-column change -> filter rebuild pipeline (Demoted /
    // Promoted / Grew branches). Extracted to a member function so
    // the 140-line body has one source of truth for both the ctor
    // install and the tab-switch reinstall (B1 fix).
    mSessionConnections +=
        connect(mModel, &LogModel::enumColumnsChanged, this, &MainWindow::OnEnumColumnsChangedApplyFilterRebuild);

    // --- Shell action -> view-owned table (Copy, follow-tail toggle,
    // anchor color hotkeys, clear-anchor, jump-to-tail pill,
    // double-click details) --------------------------------------------
    mSessionConnections +=
        connect(ui->actionCopy, &QAction::triggered, mTableView, &LogTableView::CopySelectedRowsToClipboard);
    mSessionConnections += connect(mTableView, &LogTableView::userScrolledAwayFromTail, this, [this]() {
        if (ui->actionFollowTail->isChecked())
        {
            ui->actionFollowTail->setChecked(false);
        }
    });
    mSessionConnections += connect(mTableView, &LogTableView::userScrolledToTail, this, [this]() {
        if (!ui->actionFollowTail->isChecked() && IsLiveTailSession())
        {
            ui->actionFollowTail->setChecked(true);
        }
    });
    mSessionConnections += connect(ui->actionFollowTail, &QAction::toggled, this, [this](bool checked) {
        if (mTableView != nullptr)
        {
            mTableView->SetPendingNewRowsSuppressed(checked);
        }
    });
    mSessionConnections += connect(mTableView, &LogTableView::jumpToTailRequested, this, [this]() {
        if (mTableView != nullptr)
        {
            mTableView->AcknowledgePendingNewRows();
        }
        JumpToNewestRow();
        if (IsLiveTailSession() && !ui->actionFollowTail->isChecked())
        {
            ui->actionFollowTail->setChecked(true);
        }
    });

    // --- Find-cache invalidation via the sort/filter proxy -------------
    mSessionConnections +=
        connect(mSortFilterProxyModel, &QAbstractItemModel::rowsInserted, this, &MainWindow::OnFindCacheInvalidated);
    mSessionConnections +=
        connect(mSortFilterProxyModel, &QAbstractItemModel::rowsRemoved, this, &MainWindow::OnFindCacheInvalidated);
    mSessionConnections +=
        connect(mSortFilterProxyModel, &QAbstractItemModel::layoutChanged, this, &MainWindow::OnFindCacheInvalidated);
    mSessionConnections +=
        connect(mSortFilterProxyModel, &QAbstractItemModel::modelReset, this, &MainWindow::OnFindCacheInvalidated);
    mSessionConnections +=
        connect(mSortFilterProxyModel, &QAbstractItemModel::columnsInserted, this, &MainWindow::OnFindCacheInvalidated);
    mSessionConnections +=
        connect(mSortFilterProxyModel, &QAbstractItemModel::columnsRemoved, this, &MainWindow::OnFindCacheInvalidated);
    mSessionConnections +=
        connect(mSortFilterProxyModel, &QAbstractItemModel::columnsMoved, this, &MainWindow::OnFindCacheInvalidated);
    mSessionConnections += connect(
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

    // --- Rows-shown status ---------------------------------------------
    mSessionConnections += connect(mModel, &QAbstractItemModel::rowsInserted, this, &MainWindow::UpdateRowsShownStatus);
    mSessionConnections += connect(mModel, &QAbstractItemModel::rowsRemoved, this, &MainWindow::UpdateRowsShownStatus);
    mSessionConnections += connect(mModel, &QAbstractItemModel::modelReset, this, &MainWindow::UpdateRowsShownStatus);
    mSessionConnections +=
        connect(mSortFilterProxyModel, &QAbstractItemModel::rowsInserted, this, &MainWindow::UpdateRowsShownStatus);
    mSessionConnections +=
        connect(mSortFilterProxyModel, &QAbstractItemModel::rowsRemoved, this, &MainWindow::UpdateRowsShownStatus);
    mSessionConnections +=
        connect(mSortFilterProxyModel, &QAbstractItemModel::modelReset, this, &MainWindow::UpdateRowsShownStatus);
    mSessionConnections +=
        connect(mSortFilterProxyModel, &QAbstractItemModel::layoutChanged, this, &MainWindow::UpdateRowsShownStatus);

    // --- Sort status ---------------------------------------------------
    mSessionConnections +=
        connect(mSortFilterProxyModel, &QAbstractItemModel::layoutChanged, this, &MainWindow::UpdateSortStatus);
    mSessionConnections +=
        connect(mSortFilterProxyModel, &QAbstractItemModel::modelReset, this, &MainWindow::UpdateSortStatus);
    mSessionConnections += connect(mModel, &QAbstractItemModel::rowsInserted, this, &MainWindow::UpdateSortStatus);
    mSessionConnections += connect(mModel, &QAbstractItemModel::rowsRemoved, this, &MainWindow::UpdateSortStatus);
    mSessionConnections += connect(mModel, &QAbstractItemModel::modelReset, this, &MainWindow::UpdateSortStatus);
    mSessionConnections += connect(
        mModel, &QAbstractItemModel::headerDataChanged, this, [this](Qt::Orientation orientation, int first, int last) {
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

    // --- Double-click -> details ---------------------------------------
    mSessionConnections +=
        connect(mTableView, &QAbstractItemView::doubleClicked, this, &MainWindow::ShowRecordDetailsForProxyIndex);

    // --- Anchor color hotkeys (Ctrl+1..Ctrl+9) target the active view --
    for (std::size_t i = 0; i < mAnchorColorActions.size(); ++i)
    {
        const QAction *action = mAnchorColorActions[i];
        if (action == nullptr)
        {
            continue;
        }
        const int colourIndex = static_cast<int>(i);
        mSessionConnections += connect(action, &QAction::triggered, mTableView, [view = mTableView, colourIndex]() {
            view->AnchorSelection(colourIndex);
        });
    }
    if (mActionClearRowAnchor != nullptr)
    {
        mSessionConnections +=
            connect(mActionClearRowAnchor, &QAction::triggered, mTableView, &LogTableView::ClearAnchorOnSelection);
    }

    // --- Streaming lifecycle + line/error counts + rotation flash ------
    mSessionConnections += connect(mModel, &LogModel::lineCountChanged, this, [this](qsizetype count) {
        mSession->SetStreamingLineCount(count);
        UpdateStreamingStatus();
        if (IsLiveTailSession() && !mSession->FirstStreamingBatchSeen() && count > 0)
        {
            mSession->SetFirstStreamingBatchSeen(true);
            UpdateUi();
            UpdateWindowTitle();
        }
        if (IsLiveTailSession())
        {
            ScrollToNewestRowIfFollowing();
        }
    });
    mSessionConnections += connect(mModel, &LogModel::errorCountChanged, this, [this](qsizetype count) {
        mSession->SetStreamingErrorCount(count);
        UpdateStreamingStatus();
    });
    // Completion uses only the persistent origin-bound connection to avoid duplicate handling.
    mSessionConnections += connect(mModel, &LogModel::rotationDetected, this, &MainWindow::OnRotationDetected);
    mSessionConnections += connect(mModel, &LogModel::sourceStatusChanged, this, &MainWindow::OnSourceStatusChanged);
    mSessionConnections += connect(mModel, &LogModel::columnHealthChanged, this, &MainWindow::UpdateDiagnosticsStatus);

    // --- Selection tracking for the Record Details dock (B2 fix) -------
    // Reinstall after the alias swap so the connection targets the
    // newly-bound table's selection model instead of the destroyed
    // outgoing one.
    RebindRecordDetailSelectionTracking();
}

void MainWindow::AggregateWindowModified()
{
    // A dirty background session still marks the containing window modified.
    bool anyDirty = false;
    for (const LogSession *session : hostedSessions())
    {
        if (session != nullptr && session->HasUnsavedChanges())
        {
            anyDirty = true;
            break;
        }
    }
    setWindowModified(anyDirty);
}

// Fan-out mutation of hosted-session state reads as a window mutation semantically even though every mutation flows
// through a non-owning pointer; keep non-const so the intent stays honest.
// NOLINTNEXTLINE(readability-make-member-function-const)
void MainWindow::BroadcastRotationHistoryPreference(bool enabled)
{
    // Fan the process-wide preference to every hosted session. Per-session filtering
    // (CLI per-window opt-out,
    // source-descriptor `followRotationSiblings`) happens inside
    // `LogSession::ShouldAutoDetectRotationHistory`; this method
    // simply broadcasts the "user just flipped the setting" event so
    // every session (a) drops its CLI opt-out latch and (b) mirrors
    // the value into its source descriptor.
    for (LogSession *session : hostedSessions())
    {
        if (session == nullptr)
        {
            continue;
        }
        session->SetDisableRotationHistoryOverride(false);
        auto &currentSource = session->MutableCurrentSource();
        if (!currentSource.has_value())
        {
            continue;
        }
        // Source state remains writable during teardown even when its model is unavailable.
        currentSource->followRotationSiblings = enabled;
        if (session->Model() != nullptr)
        {
            session->Model()->ConfigurationManager().SetSource(currentSource);
        }
    }
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
        qCritical().noquote() << "Fatal: failed to initialize timezone database at" << logapp::FsPathToQString(tzdata)
                              << ":" << e.what();
        return false;
    }

    initialised = true;
    return true;
}

namespace
{
// Accept only payloads containing local file URLs.
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

#ifdef LOGAPP_BUILD_TESTING
void MainWindow::DropFilesForTest(const QStringList &files, Qt::KeyboardModifiers modifiers)
{
    QMimeData mime;
    QList<QUrl> urls;
    urls.reserve(files.size());
    for (const QString &path : files)
    {
        urls.append(QUrl::fromLocalFile(path));
    }
    mime.setUrls(urls);
    QDropEvent event(QPointF(8, 8), Qt::CopyAction, &mime, Qt::LeftButton, modifiers);
    dropEvent(&event);
}
#endif

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
    case QEvent::ShortcutOverride:
    {
        // Veto the window-scope F4 shortcut when a text-editing
        // widget has focus. Accepting `ShortcutOverride` tells Qt
        // "the focus widget wants this key", and it delivers a
        // plain `KeyPress` instead of firing the shortcut. Without
        // this, F4 on a `QComboBox` (open drop-down) or spin box
        // (step) would fire the "Edit anchor note" action and the
        // key would never reach the widget for its native use.
        //
        // `AnchorsDock` is excluded because its own QAction slot
        // routes F4 to `BeginEditingCurrentNote`; letting the
        // shortcut fire there is correct.
        auto *keyEvent = static_cast<QKeyEvent *>(event); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        if (keyEvent->key() == Qt::Key_F4 && keyEvent->modifiers() == Qt::NoModifier)
        {
            const QWidget *focused = QApplication::focusWidget();
            const bool focusInAnchorsDock =
                (mAnchorsDock != nullptr && focused != nullptr && mAnchorsDock->isAncestorOf(focused));
            if (!focusInAnchorsDock && focused != nullptr &&
                (qobject_cast<const QLineEdit *>(focused) != nullptr ||
                 qobject_cast<const QTextEdit *>(focused) != nullptr ||
                 qobject_cast<const QPlainTextEdit *>(focused) != nullptr ||
                 qobject_cast<const QAbstractSpinBox *>(focused) != nullptr ||
                 qobject_cast<const QComboBox *>(focused) != nullptr))
            {
                event->accept();
                return true;
            }
        }
        break;
    }
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
    auto *child = new MainWindow(mTheme, mHistoryManager, mRegexTemplateRegistry, nullptr);
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
    mSession->ResetMode();
    // Defer the loaded sort until streaming finishes (see
    // `ApplyDeferredSortFromConfig` for the O(N^2) avoidance).
    mSession->SetPendingApplySortFromConfig(true);
    if (!DoLoadConfiguration(jsonPath))
    {
        mSession->SetPendingApplySortFromConfig(false);
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
            mSession->SetAutoSaveUuid(stem);
            // Gate the publish on (a) `Touch` succeeding (index
            // still owns the stem) and (b)
            // `RestorableActiveSessionUuid()` non-empty (the
            // session can actually be reopened on next launch -- a
            // non-restorable NetworkStream entry would create a restore
            // loop otherwise). The latch follows the bool return so
            // a contended / disabled publish doesn't claim a
            // publish that never happened.
            if (mHistoryManager->Touch(stem) && !RestorableActiveSessionUuid().isEmpty())
            {
                if (SessionHistoryManager::AddOpenWindowUuid(stem))
                {
                    mSession->SetAutoSaveUuidPublished(true);
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
        if (logapp::LooksLikeUuid(uuid))
        {
            mHistoryManager->Remove(uuid);
        }
        return;
    }

    OpenParsedSession(uuid, std::move(parsed), /*informIfNonFile=*/true);
}

void MainWindow::OpenSessionFromJson(const QString &uuid, const QString &jsonPath, bool informIfNonFile)
{
    if (jsonPath.isEmpty() || !QFileInfo::exists(jsonPath))
    {
        return;
    }

    loglib::LogConfiguration parsed;
    try
    {
        loglib::LogConfigurationManager probe;
        probe.Load(jsonPath.toStdString());
        parsed = probe.Configuration();
    }
    catch (const std::exception &)
    {
        return;
    }

    OpenParsedSession(uuid, std::move(parsed), informIfNonFile);
}

void MainWindow::OpenParsedSession(const QString &uuid, loglib::LogConfiguration parsed, bool informIfNonFile)
{
    // Preserve a busy active tab when opening a recent session. When the active tab
    // was already empty, we still route through `NewSession` to
    // detach the previous uuid and refresh the empty slot.
    const bool activeTabWasEmpty = (mSession != nullptr) && !mSession->CurrentSource().has_value() &&
                                   (mModel == nullptr || mModel->rowCount(QModelIndex{}) == 0);
    EnsureFreshActiveTab();
    if (activeTabWasEmpty)
    {
        // Replace-in-place tears down current session/model state.
        // `NewSession` also detaches our previous uuid; we re-pin
        // below via `Touch` inside the streaming setup.
        NewSession();
    }

    // Defer the loaded sort until streaming finishes (see
    // `ApplyDeferredSortFromConfig`). Set *after* `NewSession`/
    // `AddNewTab`, which clear the latch on the target session.
    mSession->SetPendingApplySortFromConfig(true);

    // Apply failure surfaces a `QMessageBox`; bail without queueing
    // files so the view is at least empty rather than mixed.
    if (!ApplyLoadedConfiguration(std::move(parsed)))
    {
        mSession->SetPendingApplySortFromConfig(false);
        return;
    }

    // Pin the uuid before streaming. Publish gated by `Touch`
    // (index still owns @p uuid) and `RestorableActiveSessionUuid`
    // (loaded session is round-trippable -- non-restorable NetworkStream
    // snapshots would otherwise create a fan-restore loop). The
    // latch follows the bool return; see `RestoreLastSessionFromPath`.
    mSession->SetAutoSaveUuid(uuid);
    if (mHistoryManager != nullptr && mHistoryManager->Touch(uuid) && !RestorableActiveSessionUuid().isEmpty())
    {
        if (SessionHistoryManager::AddOpenWindowUuid(uuid))
        {
            mSession->SetAutoSaveUuidPublished(true);
        }
    }

    StreamFromCurrentSourceOrSkip(informIfNonFile);
}

void MainWindow::StreamFromCurrentSourceOrSkip(bool informIfNonFile)
{
    if (!loglib::HasLocators(mSession->MutableCurrentSource()))
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
    const auto &source = *mSession->MutableCurrentSource();
    if (source.kind != loglib::LogConfiguration::Source::Kind::File)
    {
        // Network and stdin locators cannot be reopened. Stdin can
        // still reach this branch through a manually created or older bundle.
        if (informIfNonFile)
        {
#ifdef LOGAPP_BUILD_TESTING
            if (!mSuppressDialogsForTest)
#endif
            {
                const bool isStdin = source.kind == loglib::LogConfiguration::Source::Kind::Stdin;
                const QString title =
                    isStdin ? QStringLiteral("Standard Input Session") : QStringLiteral("Network Stream Session");
                const QString body =
                    isStdin ? QStringLiteral(
                                  "This saved session includes a stdin source; the columns and filters have been "
                                  "restored, but stdin cannot be reopened. Relaunch with `-` or `--stdin` to "
                                  "read from a new pipe."
                              )
                            : QStringLiteral(
                                  "This recent session was a network stream; the columns and filters have been "
                                  "restored, but the producer must be re-bound manually via 'Open Network Stream...'."
                              );
                QMessageBox::information(this, title, body);
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
    // New Session uses the same close-decision model as Close Tab.
    if (mSession != nullptr && !PrepareSessionClose(mSession))
    {
        return;
    }

    // Tear down all loaded state -- rows, filters, source, session
    // mode, columns, sort -- so the window matches "blank window"
    // semantics. `LogModel::Reset` handles producer stop + sink
    // drain for live-tail sessions, no extra branch needed.

    // Cancel export and decompression before `mModel->Reset()` so a
    // finishing worker cannot continue the open pipeline against a
    // cleared model. Safe to call when nothing is in flight.
    CancelInFlightExport();
    CancelInFlightDecompression();

    // Drop proxy state before the configuration wipe so no signal
    // handler can briefly evaluate against indices that become
    // dangling once `columns` is empty.
    mTableView->sortByColumn(-1, Qt::AscendingOrder);
    mSortFilterProxyModel->SetFilterExpression(loglib::CompiledFilterExpression{});

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

    mSession->MutableCurrentSource().reset();
    mSession->ResetMode();
    mSession->ResetStreamingCountersAndFileName();
    // Drop the outgoing session's multi-file queue too -- otherwise
    // queued-but-not-yet-drained files stay invisibly attached to a
    // session that no longer exists until the next destructive open
    // overwrites them. `OpenLogStreamFromPath` / `OpenNetworkStream`
    // clear the queue at their seams for the same reason.
    mSession->ClearPendingOpenQueues();
    // Prevent pending tail and Undo state from crossing sessions.
    ClearPendingLiveTailPromotion();
    ClearRotationExpansionUndoState();
    // The effective preference no longer has a session-level gate.
    SyncRotationHistoryActionCheckedState();
    // The configuration that requested the deferred sort is gone.
    mSession->SetPendingApplySortFromConfig(false);
    // Drop the Goto Timestamp / Goto Line sticky inputs: a value
    // that made sense in the outgoing session (zone, format, row
    // count) can be nonsense in a new one, and pre-populating the
    // dialog with it just annoys the user. The state belongs to the view.
    if (mSessionView != nullptr)
    {
        mSessionView->ClearGotoStickyInputs();
    }
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
        // Compressed variants of every supported log extension. The
        // sniff is content-based, but hiding `.gz` / `.bz2` / `.xz`
        // / `.zst` would force users through "All Files (*.*)" for
        // the common rotated-log case. Live-tail (`Open Log Stream…`)
        // is not extended: compressed live-tail is a v1 non-goal.
        tr("Structured Logs (*.json *.jsonl *.ndjson *.logfmt *.csv *.log *.txt "
           "*.json.gz *.jsonl.gz *.ndjson.gz *.logfmt.gz *.csv.gz *.log.gz *.txt.gz "
           "*.json.bz2 *.jsonl.bz2 *.ndjson.bz2 *.logfmt.bz2 *.csv.bz2 *.log.bz2 *.txt.bz2 "
           "*.json.xz *.jsonl.xz *.ndjson.xz *.logfmt.xz *.csv.xz *.log.xz *.txt.xz "
           "*.json.zst *.jsonl.zst *.ndjson.zst *.logfmt.zst *.csv.zst *.log.zst *.txt.zst "
           "*.gz *.bz2 *.xz *.zst);;All Files (*.*)")
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
        mSortFilterProxyModel->SetFilterExpression(loglib::CompiledFilterExpression{});
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
        mSession->MutableCurrentSource() = mModel->Configuration().source;
        logapp::BackfillLocatorDedupKeys(mSession->MutableCurrentSource());
        // Loaded source preferences replace the prior session's Undo state.
        ClearRotationExpansionUndoState();
        SyncRotationHistoryActionCheckedState();

        // Bulk-replace anchors before RebuildFiltersFromConfiguration
        // mirrors the (now empty) `AnchorManager` back onto disk.
        // Newer-schema colour slots are clamped (not dropped) so
        // the bookmark + note survive a downgrade; the remap count
        // is surfaced to the user.
        if (mAnchors != nullptr)
        {
            const std::size_t clampedAnchorCount = mAnchors->Replace(mModel->Configuration().anchors);
            if (clampedAnchorCount > 0)
            {
                statusBar()->showMessage(
                    tr("%1 anchor(s) from a newer schema had their colour clamped to slot %2.")
                        .arg(static_cast<qulonglong>(clampedAnchorCount))
                        .arg(static_cast<qulonglong>(loglib::ANCHOR_PALETTE_SIZE)),
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

void MainWindow::ShowRotationHistoryToast(int addedCount, const QString &primary)
{
    if (addedCount <= 0)
    {
        return;
    }
    const QString baseName = primary.isEmpty() ? tr("primary log") : QFileInfo(primary).fileName();
    statusBar()->showMessage(
        tr("Loaded %n rotated companion(s) alongside %1. Undo via Settings \u25b8 Undo rotated history expansion.",
           nullptr,
           addedCount)
            .arg(baseName),
        STATUS_BAR_MESSAGE_TIMEOUT_MS
    );
    if (mActionUndoRotationExpansion != nullptr)
    {
        mActionUndoRotationExpansion->setEnabled(true);
    }
}

void MainWindow::UndoRotationExpansion()
{
    if (mSession->LastRotationExpansionOriginalInputs().isEmpty())
    {
        return;
    }
    // Clear Undo state before reopening to avoid carrying it across reset.
    // Copy first; `ClearRotationExpansionUndoState` invalidates the
    // reference returned by `LastRotationExpansionOriginalInputs()`.
    const QStringList originalInputs = mSession->LastRotationExpansionOriginalInputs();
    const bool wasLiveTail = mSession->LastRotationExpansionWasLiveTail();
    ClearRotationExpansionUndoState();
    Q_ASSERT(!originalInputs.isEmpty());
    // Temporarily suppress expansion while reopening the original inputs.
    const bool priorOverride = mSession->DisableRotationHistoryOverride();
    mSession->SetDisableRotationHistoryOverride(true);
    if (wasLiveTail)
    {
        // Preserve live-tail mode; this path captures exactly one input.
        OpenLogStreamFromPath(originalInputs.first());
    }
    else
    {
        // Restore the complete multi-file selection.
        DispatchMixedOpenInput(originalInputs, OpenMode::Replace);
    }
    mSession->SetDisableRotationHistoryOverride(priorOverride);
}

void MainWindow::SyncRotationHistoryActionCheckedState()
{
    if (mActionAutoDetectRotationHistory == nullptr)
    {
        return;
    }
    const QSignalBlocker blocker(mActionAutoDetectRotationHistory);
    mActionAutoDetectRotationHistory->setChecked(EffectiveAutoDetectRotationHistory());
}

void MainWindow::ClearRotationExpansionUndoState() noexcept
{
    mSession->ClearRotationExpansionUndoState();
    if (mActionUndoRotationExpansion != nullptr)
    {
        mActionUndoRotationExpansion->setEnabled(false);
    }
}

void MainWindow::ClearPendingLiveTailPromotion() noexcept
{
    mSession->ClearPendingLiveTailPromotion();
}

void MainWindow::SetRotationHistoryLaunchOverride(bool disable)
{
    mSession->SetDisableRotationHistoryOverride(disable);
    SyncRotationHistoryActionCheckedState();
}

void MainWindow::OnRotationHistoryPrefToggled(bool enabled)
{
    QSettings settings;
    // Avoid redundant backend writes and resulting change notifications.
    const bool previousStored = settings.value(QStringLiteral("ui/autoDetectRotatedHistory"), true).toBool();
    if (previousStored != enabled)
    {
        settings.setValue(QStringLiteral("ui/autoDetectRotatedHistory"), enabled);
    }

    // Fan the preference change to every hosted session. Each session drops its CLI
    // opt-out latch and mirrors the value into its source
    // descriptor. Previously this only touched `mSession`, which
    // meant a background tab kept its stale preference on multi-tab
    // windows.
    BroadcastRotationHistoryPreference(enabled);
    SyncRotationHistoryActionCheckedState();
}

bool MainWindow::ShouldAutoDetectRotationHistory() const
{
    return mSession->ShouldAutoDetectRotationHistory();
}

bool MainWindow::EffectiveAutoDetectRotationHistory() const
{
    return mSession->EffectiveAutoDetectRotationHistory();
}

QStringList MainWindow::ExpandLogPathsWithRotationSiblings(
    const QStringList &logPaths, int &addedOut, RotationSourceGating gating, QString *primaryOut
) const
{
    addedOut = 0;
    if (primaryOut != nullptr)
    {
        primaryOut->clear();
    }
    if (logPaths.isEmpty() || !ShouldAutoDetectRotationHistory())
    {
        return logPaths;
    }

    // Opt-out and dedup gates are separate because a loaded
    // configuration retains its preference after its rows reset,
    // while its locator keys no longer describe visible rows.
    const bool consultOptOut = (gating != RotationSourceGating::Ignore);
    const bool consultDedup = (gating == RotationSourceGating::HonourAll);

    const auto &currentSource = mSession->CurrentSource();
    // Honour a loaded session's opt-out when requested.
    if (consultOptOut && currentSource.has_value() && !currentSource->followRotationSiblings)
    {
        return logPaths;
    }

    // Deduplicate only against locators still visible in this session.
    std::unordered_set<std::string> alreadyLoaded;
    if (consultDedup && currentSource.has_value())
    {
        alreadyLoaded.reserve(currentSource->locatorDedupKeys.size());
        for (const std::string &k : currentSource->locatorDedupKeys)
        {
            alreadyLoaded.insert(k);
        }
    }

    // Bundles bypass rotation-family expansion.
    QStringList expanded;
    expanded.reserve(logPaths.size());

    std::vector<std::filesystem::path> fsPaths;
    fsPaths.reserve(logPaths.size());
    QStringList nonExpandableInputs;
    nonExpandableInputs.reserve(logPaths.size());
    for (const QString &p : logPaths)
    {
        if (p.isEmpty())
        {
            nonExpandableInputs.append(p);
            continue;
        }
        if (IsSessionBundlePath(p) && loglib::LooksLikeSessionBundle(logapp::QStringToFsPath(p)))
        {
            nonExpandableInputs.append(p);
            continue;
        }
        fsPaths.push_back(logapp::QStringToFsPath(p));
    }

    if (fsPaths.empty())
    {
        return logPaths;
    }

    const loglib::PartitionedSelection partitioned =
        loglib::PartitionAsRotationSeries(std::span<const std::filesystem::path>(fsPaths));

    std::unordered_set<std::string> emittedKeys;
    emittedKeys.reserve(fsPaths.size());

    // Count only paths absent from the caller's canonical input set;
    // caller-selected companions must not enable the expansion Undo.
    std::unordered_set<std::string> userSelectedKeys;
    userSelectedKeys.reserve(fsPaths.size());
    for (const std::filesystem::path &fp : fsPaths)
    {
        userSelectedKeys.insert(loglib::CanonicalKeyForPath(fp));
    }

    // Emit each series oldest-first while preserving family order.
    for (const loglib::RotationSeries &series : partitioned.series)
    {
        int seriesAdded = 0;
        // Keep the actual family primary for the status message.
        const QString seriesPrimaryDisplay = logapp::CanonicalDisplayPath(logapp::FsPathToQString(series.primary));
        for (const loglib::RotatedFile &rf : series.files)
        {
            if (alreadyLoaded.contains(rf.canonicalKey))
            {
                continue;
            }
            if (!emittedKeys.insert(rf.canonicalKey).second)
            {
                continue;
            }
            // Preserve native Windows encoding when returning paths to Qt.
            const QString display = logapp::CanonicalDisplayPath(logapp::FsPathToQString(rf.path));
            expanded.append(display);
            // Count only detected siblings not explicitly selected.
            const bool autoDiscoveredByRotationDetection = rf.origin == loglib::RotatedFile::Origin::NumberedSuffix ||
                                                           rf.origin == loglib::RotatedFile::Origin::DatedSuffix;
            if (autoDiscoveredByRotationDetection && !userSelectedKeys.contains(rf.canonicalKey))
            {
                ++seriesAdded;
            }
        }
        addedOut += seriesAdded;
        // Use the first family that contributed an added sibling.
        if (primaryOut != nullptr && seriesAdded > 0 && primaryOut->isEmpty())
        {
            *primaryOut = seriesPrimaryDisplay;
        }
    }

    // Residual paths (no siblings, unrelated files) preserve order.
    for (const std::filesystem::path &rp : partitioned.residual)
    {
        const std::string key = loglib::CanonicalKeyForPath(rp);
        if (alreadyLoaded.contains(key))
        {
            continue;
        }
        if (!emittedKeys.insert(key).second)
        {
            continue;
        }
        expanded.append(logapp::CanonicalDisplayPath(logapp::FsPathToQString(rp)));
    }

    // Reattach non-expandable inputs for the upstream classifier.
    for (const QString &p : nonExpandableInputs)
    {
        expanded.append(p);
    }

    return expanded;
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
    //
    // Recognize bundles before probing uncompressed configuration JSON.
    QStringList configPaths;
    QStringList logPaths;
    configPaths.reserve(files.size());
    logPaths.reserve(files.size());
    int bundleCount = 0;
    for (const QString &file : files)
    {
        if (file.isEmpty())
        {
            continue;
        }
        if (IsSessionBundlePath(file) && loglib::LooksLikeSessionBundle(logapp::QStringToFsPath(file)))
        {
            ++bundleCount;
            logPaths.append(file);
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
        // Sole bundle: arm embedded-configuration apply for that path.
        // Non-arming opens leave any pending intent untouched.
        const bool armEmbeddedBundleIntent =
            bundleCount == 1 && logPaths.size() == 1 &&
            (logMode == OpenMode::Replace ||
             (mModel->rowCount() == 0 && !mSession->MutableCurrentSource().has_value()));
        // The gate guarantees the sole log path is the bundle.
        const QString bundlePath = armEmbeddedBundleIntent ? logPaths.front() : QString();

        // Snapshot the effective preference before replacement clears
        // the current source. Replacement ignores outgoing locator
        // keys; append retains both session opt-out and deduplication.
        const bool wantRotationHistoryForReplace =
            (logMode != OpenMode::Replace) || EffectiveAutoDetectRotationHistory();
        const RotationSourceGating logOnlyGating =
            (logMode == OpenMode::Append) ? RotationSourceGating::HonourAll : RotationSourceGating::Ignore;
        int addedSiblings = 0;
        QString expandedPrimary;
        const QStringList expandedLogPaths =
            (armEmbeddedBundleIntent || !wantRotationHistoryForReplace)
                ? logPaths
                : ExpandLogPathsWithRotationSiblings(logPaths, addedSiblings, logOnlyGating, &expandedPrimary);
        // Destructive queue setup cancels a superseded decompression
        // and clears its bundle intent. Arm the new path after that
        // reset and before the first file starts: a small bundle can
        // finish inside watcher `setFuture`, and
        // `OnDecompressionFinished` reads the intent then.
        StartStreamingOpenQueue(expandedLogPaths, logMode, bundlePath);
        if (addedSiblings > 0)
        {
            // Set after queue setup, whose replacement path clears Undo
            // state. Preserve every caller input for multi-file Undo.
            mSession->SetLastRotationExpansion(logPaths, /*wasLiveTail=*/false);
            const QString anchor = expandedPrimary.isEmpty() ? logPaths.back() : expandedPrimary;
            ShowRotationHistoryToast(addedSiblings, anchor);
        }
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
    mSession->SetPendingApplySortFromConfig(true);
    if (!DoLoadConfiguration(configPath))
    {
        // TOCTOU: the file was rewritten between probe and commit.
        // The model was reset before the throw, so streaming logs
        // against the unintended default columns would mislead --
        // bail without queueing anything.
        mSession->SetPendingApplySortFromConfig(false);
        return MixedInputResult{.outcome = MixedInputDispatch::QueuedLogsOnly, .appliedConfigPath = QString()};
    }
    int addedSiblings = 0;
    QString expandedPrimary;
    // Keep the loaded configuration's opt-out, but ignore locator
    // keys from rows that `DoLoadConfiguration` reset.
    const QStringList expandedLogPaths = ExpandLogPathsWithRotationSiblings(
        logPaths, addedSiblings, RotationSourceGating::HonourOptOutOnly, &expandedPrimary
    );
    StartStreamingOpenQueue(expandedLogPaths, OpenMode::Append);
    if (addedSiblings > 0)
    {
        // Queue setup may tear down a live tail, so set Undo afterward.
        mSession->SetLastRotationExpansion(logPaths, /*wasLiveTail=*/false);
        const QString anchor = expandedPrimary.isEmpty() ? logPaths.back() : expandedPrimary;
        ShowRotationHistoryToast(addedSiblings, anchor);
    }
    return MixedInputResult{.outcome = MixedInputDispatch::AppliedConfigThenLogs, .appliedConfigPath = configPath};
}

void MainWindow::StartStreamingOpenQueue(QStringList files, OpenMode mode, const QString &applyEmbeddedBundlePath)
{
    // Live-tail / network sessions are single-source: a new
    // static-files open implicitly tears them down regardless of
    // `mode`. Static files (plain, compressed, or bundle) all follow
    // the normal append/replace queue semantics.
    const bool destructive = (mode == OpenMode::Replace) || (mSession->SessionMode() == SessionMode::LiveTail);
    if (destructive && !PrepareSessionClose(mSession))
    {
        return;
    }

    // Cancel export for every `mode`. The destructive branch also
    // cancels decompression before `mModel->Reset()` so a finishing
    // worker cannot continue against a cleared model. The
    // non-destructive Append branch still needs the export cancel
    // because `AppendStreaming` mutates `LogTable` / `KeyIndex`
    // while the export is still reading them. `setEnabled(false)`
    // blocks that path from the GUI but cross-instance CLI
    // forwarding (`OpenFilesForCli`) bypasses it. Safe to call when
    // nothing is in flight.
    CancelInFlightExport();

    if (destructive)
    {
        CancelInFlightDecompression();
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
        mSession->SetStreamingErrorsCut(0);
        mSession->MutableCurrentSource().reset();
        mSession->ResetMode();
        // Destructive open: drop the previous session's deferred-sort intent.
        mSession->SetPendingApplySortFromConfig(false);
        DetachAutoSaveUuid();
        // Pending promotion and Undo state belong to the old session.
        ClearPendingLiveTailPromotion();
        ClearRotationExpansionUndoState();
        SyncRotationHistoryActionCheckedState();
    }
    else if (mModel->IsStreamingActive() || mSession->IsDecompressionInFlight())
    {
        // Append onto an in-flight static session: queue and let
        // the existing `streamingFinished` -> `StreamNextPendingFile`
        // chain drain it. Starting another worker here would trip
        // `LogModel::AppendStreaming`'s watcher assert.
        //
        // The `mSession->IsDecompressionInFlight()` half covers the window
        // between `BeginAsyncDecompression` and the first
        // `BeginStreaming` inside `ContinueOpenAfterPrepared` (before
        // `IsStreamingActive` flips). Without it, an Append landing
        // mid-decompression on an empty model would race to
        // `BeginStreaming` (row loss when the decompression later
        // resets the model) or trip the `AppendStreaming` assert.
        mSession->MutablePendingOpenFiles().append(std::move(files));
        if (!applyEmbeddedBundlePath.isEmpty())
        {
            mSession->SetApplyEmbeddedBundleConfigForPath(applyEmbeddedBundlePath);
        }
        return;
    }
    else if (mSession->SessionMode() == SessionMode::Idle && mModel->rowCount() > 0)
    {
        // Append into a previously-finished static session: re-arm
        // `Static` so `StreamNextPendingFile` routes through
        // `AppendStreaming` instead of the row-clearing
        // `BeginStreaming` path.
        mSession->SetMode(SessionMode::Static);
    }
    // Otherwise (Idle + empty model): leave mode at Idle so the
    // first `StreamNextPendingFile` takes the `BeginStreaming` path,
    // which preserves runtime filters from a prior
    // "Load Configuration or Session...".

    mSession->SetPendingOpenFiles(std::move(files));
    // Reset in parallel with `SetPendingOpenFiles` above; the error
    // buckets drain at the same seams under different titles.
    mSession->ClearPendingOpenErrors();
    mSession->ClearPendingDecompressionErrors();
    if (!applyEmbeddedBundlePath.isEmpty())
    {
        mSession->SetApplyEmbeddedBundleConfigForPath(applyEmbeddedBundlePath);
    }

    StreamNextPendingFile(mSession);
}

void MainWindow::OnStreamingFinished(LogSession *origin, StreamingResult result)
{
    if (origin == nullptr)
    {
        return;
    }
    LogModel *const model = origin->Model();
    if (model == nullptr)
    {
        return;
    }

    // Skip outgoing-session UI cleanup when we're mid session-
    // switch (the synchronous `Cancelled` emitted by `LogModel::Reset`
    // would otherwise flicker the wrong toolbar / status state at
    // the user). The outer caller finishes UI wiring itself.
    if (result == StreamingResult::Cancelled && origin->IsSessionSwitchInProgress())
    {
        return;
    }

    // Clear the `Source unavailable` latch.
    origin->SetSourceWaiting(false);

    // Per-file batch under a header that names the file (or stream)
    // that just finished. Fires *before* the chaining check so the
    // intermediate file's errors don't get folded into the last
    // file's batch in a multi-file static open. `mStreamingFileName`
    // is still set to the just-finished source here -- the chaining
    // path below will overwrite it for the next file.
    //
    // Open-failure entries in `origin->MutablePendingOpenErrors()` are intentionally
    // *not* folded in here: they aren't tied to any one streamed file
    // and would be misleading under a file-named header. They're
    // surfaced under their own batch at the bottom of this function.
    if (result == StreamingResult::Success)
    {
        const auto &allErrors = model->StreamingErrors();
        // `std::min` guards against a model reset that landed between
        // our last slice and now (would leave the watermark past end).
        const size_t cut = std::min(origin->StreamingErrorsCut(), allErrors.size());
        if (cut < allErrors.size())
        {
            const std::vector<std::string> thisFileErrors(
                allErrors.begin() + static_cast<std::ptrdiff_t>(cut), allErrors.end()
            );
            const QString title = origin->StreamingFileName().isEmpty()
                                      ? tr("Error Parsing Logs")
                                      : tr("Error Parsing Logs \u2014 %1").arg(origin->StreamingFileName());
            ShowParseErrors(title, thisFileErrors, origin);
        }
        origin->SetStreamingErrorsCut(allErrors.size());
    }

    // Multi-file static open: Success advances the queue. Keep
    // `SessionMode::Static` across `StreamNextPendingFile` so
    // it routes follow-up files through `AppendStreaming`.
    if (result == StreamingResult::Success && !origin->MutablePendingOpenFiles().isEmpty())
    {
        StreamNextPendingFile(origin);
        // `IsStreamingActive()` covers the uncompressed fast path;
        // `origin->IsDecompressionInFlight()` covers the case where the next
        // file is compressed and `StreamNextPendingFile` only
        // dispatched the async decompression worker. Without the
        // second half we'd fall through to session teardown, and
        // `OnDecompressionFinished` would later see
        // `!origin->IsSessionActive()` and take the row-clearing
        // `BeginStreaming` path -- discarding every earlier file's
        // rows.
        if (model->IsStreamingActive() || origin->IsDecompressionInFlight())
        {
            return;
        }
    }
    else if (!origin->MutablePendingOpenFiles().isEmpty())
    {
        origin->MutablePendingOpenFiles().clear();
    }

    // Promote a requested tail after any terminal prefix result;
    // destructive session changes clear the pending primary first.
    //
    // Always defer promotion. `StopAndKeepRows` may dispatch the
    // sink's queued completion from inside its teardown, re-entering
    // this slot. Re-arming synchronously there lets the outer teardown
    // drop the new generation's batches and emit a false cancellation.
    // Queuing preserves the required teardown-before-arm ordering.
    //
    // Capture the origin with `QPointer` so a tab switch between the queue and the
    // invocation does not promote a sibling session's pending
    // primary against the newly-active tab. If the origin session
    // has been destroyed by then (tab closed) the callback silently
    // no-ops.
    if (!origin->PendingLiveTailPrimary().isEmpty())
    {
        QMetaObject::invokeMethod(
            this,
            [this, originSession = QPointer<LogSession>(origin)]() {
                if (originSession.isNull() || HostedSession(originSession->InstanceId()) != originSession.data())
                {
                    return;
                }
                ContinueLiveTailAfterPrefix(originSession.data());
            },
            Qt::QueuedConnection
        );
        return;
    }

    // Snapshot the mode before resetting so the auto-save gate
    // distinguishes static (worth saving) from live-tail / network
    // (transient). `LogSession::SetMode(Idle)` latches the previous
    // mode into `LastTerminalMode()` automatically so a later
    // closeEvent flush still sees the terminated kind.
    const SessionMode justFinishedMode = origin->SessionMode();
    origin->SetMode(SessionMode::Idle);

    // Shell-global chrome belongs to the selected tab. Origin state
    // (mode, source, parse errors) is updated above regardless.
    const bool originIsActive = origin == mSession;
    if (originIsActive)
    {
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
    }
    // Refresh the column-health snapshot now that parsing has
    // settled. Drives the header warning glyph and the status-bar
    // mismatch summary via `columnHealthChanged`.
    model->RefreshColumnHealth();
    // Per-file parse-error batches were already surfaced at the top
    // of this function (one per file in the chain). What remains is
    // any open-failure residue from the multi-file queue -- those
    // entries are tied to files that never streamed, so they get
    // their own dedicated batch instead of being mislabeled under
    // a streamed-file header.
    //
    // Drained on any terminal result (Success / Failed / Cancelled)
    // rather than Success-only, so a Failed / Stop that follows an
    // earlier "Failed to open" entry doesn't silently drop that
    // entry. Destructive session-switch cancels already returned at
    // the top of this function, so this only runs for user-facing
    // completions.
    if (!origin->MutablePendingOpenErrors().empty())
    {
        ShowParseErrors(tr("Error Opening File"), origin->MutablePendingOpenErrors(), origin);
    }
    origin->MutablePendingOpenErrors().clear();
    // Decompression errors under their own title (see the
    // `LogSession::MutablePendingDecompressionErrors` doc). User cancels never land
    // here -- they surface as a toast in `OnDecompressionFinished`.
    if (!origin->MutablePendingDecompressionErrors().empty())
    {
        ShowParseErrors(tr("Error Decompressing File"), origin->MutablePendingDecompressionErrors(), origin);
    }
    origin->MutablePendingDecompressionErrors().clear();
    origin->ClearStreamingFileName();
    // Keep `origin->MutableCurrentSource()` on Success / Cancelled (rows are
    // still present, descriptor still describes them); drop it
    // on Failed where there is nothing left to describe.
    if (result == StreamingResult::Failed)
    {
        origin->MutableCurrentSource().reset();
        // Failure here is on the parse worker, not the decompression
        // worker, but defensively cancel any decompression that
        // might be queued behind the failed parse so it doesn't
        // inject into the teardown that follows. Per-file anchor
        // lifetimes stay attached to their LogFiles; a session-wide
        // clear here would risk unlinking temp files still backing
        // mmap'd rows from previously-successful appends.
        CancelInFlightDecompressionFor(origin);
    }

    // Terminal transitions may change the effective session preference.
    if (originIsActive)
    {
        SyncRotationHistoryActionCheckedState();
    }

    // Apply the deferred sort before the auto-save below so the
    // mirror reads the applied sort from the proxy. Runs on every
    // terminal result; no-op when the latch is clear or the user
    // sorted mid-stream.
    ApplyDeferredSortFromConfig(origin, LogSessionViewForSession(origin));

    // Auto-save on success so Recent Sessions + restore-on-launch
    // can reopen this view. `ShouldAutoSaveSession` filters out
    // non-restorable shapes (no manager, no source, streams,
    // live-tail).
    if (result == StreamingResult::Success && ShouldAutoSaveSession(origin, justFinishedMode))
    {
        AutoSaveSessionSnapshot(origin, /*publishOpenWindow=*/true);
    }
}

void MainWindow::StreamNextPendingFile(LogSession *origin)
{
    if (origin == nullptr)
    {
        return;
    }
    // Iterative queue drain: each iteration hands off async work
    // (returning to await the corresponding finished slot) or
    // records a synchronous open failure and continues. The loop
    // replaces a prior recursion pattern that was stack-unbounded
    // on drops with many failing files.
    while (!origin->MutablePendingOpenFiles().isEmpty())
    {
        const QString file = origin->MutablePendingOpenFiles().takeFirst();

        // Content-based codec sniff (single source of truth for the
        // codec table). Uncompressed / empty / unreadable paths
        // return `None` and take the fast path below; genuine open
        // failures surface via `LogFile`'s ctor further down.
        //
        // Runs on the GUI thread. 6 bytes off a local disk is
        // sub-millisecond, but a stalled network share can block.
        // `LogFile`'s open below has the same property, so this
        // isn't the tightest link -- if remote I/O becomes a
        // supported use-case, fold both into the async worker.
        // Preserve non-ASCII Windows paths during codec detection and open.
        const std::filesystem::path filePath = logapp::QStringToFsPath(file);
        const auto codec = loglib::internal::DecompressingByteSource::SniffCodec(filePath);
        if (codec != loglib::internal::DecompressingByteSource::Codec::None)
        {
            // Compressed: dispatch async so the GUI stays responsive.
            // The finished slot re-enters this function after the
            // parse hand-off (success) or after pushing the error
            // and continuing (failure).
            BeginAsyncDecompression(origin, file, codec);
            return;
        }

        // Uncompressed fast path. On sync error (`false` return),
        // continue draining the queue.
        if (ContinueOpenAfterPrepared(origin, file, filePath, nullptr))
        {
            return;
        }
    }

    // Queue drained without a session ever arming: surface errors
    // now. When a session did arm, `OnStreamingFinished` drains them.
    if (!origin->IsSessionActive())
    {
        // If every prefix file fails synchronously, no completion
        // signal can promote the requested tail. Do it before errors.
        if (!origin->PendingLiveTailPrimary().isEmpty())
        {
            ContinueLiveTailAfterPrefix(origin);
        }
        if (!origin->MutablePendingOpenErrors().empty())
        {
            ShowParseErrors(tr("Error Opening File"), origin->MutablePendingOpenErrors(), origin);
            origin->MutablePendingOpenErrors().clear();
        }
        if (!origin->MutablePendingDecompressionErrors().empty())
        {
            ShowParseErrors(tr("Error Decompressing File"), origin->MutablePendingDecompressionErrors(), origin);
            origin->MutablePendingDecompressionErrors().clear();
        }
    }
}

bool MainWindow::ContinueOpenAfterPrepared(
    LogSession *origin,
    const QString &originalPath,
    const std::filesystem::path &effectivePath,
    std::shared_ptr<loglib::internal::DecompressingByteSource> decompressionAnchor
)
{
    if (origin == nullptr)
    {
        return false;
    }
    LogModel *const model = origin->Model();
    if (model == nullptr)
    {
        return false;
    }
    const bool originIsActive = origin == mSession;

    // Open on the GUI thread so any I/O failure surfaces alongside
    // the queue drain rather than through the async future.
    std::unique_ptr<loglib::LogFile> logFile;
    try
    {
        logFile = std::make_unique<loglib::LogFile>(effectivePath, logapp::QStringToFsPath(originalPath));
    }
    catch (const std::exception &e)
    {
        // Route the failure to the bucket that matches the cause:
        //   - No anchor           -> "Error Opening File"
        //   - Anchor (decompress) -> "Error Decompressing File"
        // A decompressed-temp mmap failure is rare (transient FS
        // pressure, AV) but labelling it as a plain open error
        // would misdirect a user who handed us a `.gz`.
        const std::string msg = std::string("Failed to open '") + originalPath.toStdString() + "': " + e.what();
        if (decompressionAnchor)
        {
            origin->MutablePendingDecompressionErrors().push_back(msg);
        }
        else
        {
            origin->MutablePendingOpenErrors().push_back(msg);
        }
        // `decompressionAnchor` goes out of scope on return, dropping
        // the last reference to the `DecompressingByteSource` which
        // deletes the temp file.
        //
        // Return `false` so the caller continues draining the queue.
        return false;
    }

    // Bind the anchor to this specific LogFile: on destruction, mmap
    // unmaps first (declaration order in LogFile), then the anchor's
    // dtor unlinks the temp file -- the Windows-safe order. A
    // session-scoped vector clear could yank a temp file out from
    // under still-live mmaps. See `AttachLifetimeAnchor` for the
    // ordering contract.
    if (decompressionAnchor)
    {
        logFile->AttachLifetimeAnchor(std::move(decompressionAnchor));
    }

    auto cfg = std::make_shared<const loglib::LogConfiguration>(model->Configuration());

    const bool isFirstFileInSession = !origin->IsSessionActive();

    origin->SetStreamingFileName(QFileInfo(originalPath).fileName());
    // Record every appended file in load order so SaveSession +
    // Recent Sessions can reopen the full set. Two forms per locator:
    //   - `displayPath`: original case, slash-normalised (user-visible).
    //   - `dedupKey`: lower-cased on Windows for byte-equality dedup.
    // Both derived from the ORIGINAL (compressed) path so a saved
    // session reopens the `.gz` and re-runs decompression fresh --
    // temp paths never enter the locator vectors.
    const std::string displayPath = logapp::CanonicalDisplayPath(originalPath).toStdString();
    const std::string dedupKey = logapp::CanonicalLocator(originalPath).toStdString();
    auto &currentSource = origin->MutableCurrentSource();
    if (isFirstFileInSession)
    {
        DetectedFormat detected = DetectFormatForPath(effectivePath);
        currentSource = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File,
            .format = detected.format,
            .locators = {displayPath},
            .locatorDedupKeys = {dedupKey},
            .regexPattern = std::move(detected.regexPattern),
        };
        // Seed the source from the global preference and CLI override.
        currentSource->followRotationSiblings = ShouldAutoDetectRotationHistory();
        if (originIsActive)
        {
            SyncRotationHistoryActionCheckedState();
        }
    }
    else if (currentSource.has_value() && currentSource->kind == loglib::LogConfiguration::Source::Kind::File)
    {
        const bool alreadyPresent = std::any_of(
            currentSource->locatorDedupKeys.begin(),
            currentSource->locatorDedupKeys.end(),
            [&dedupKey](const std::string &existing) { return existing == dedupKey; }
        );
        if (!alreadyPresent)
        {
            loglib::AppendLocator(*currentSource, displayPath, dedupKey);
        }
    }
    if (isFirstFileInSession)
    {
        origin->SetMode(SessionMode::Static);
        origin->ResetStreamingProgress();
        // Gate shell-global writes so a background-tab completion
        // does not disable the visible tab's menus / toolbar.
        // `ApplyDisplayOrder` targets the origin's proxy and view.
        if (originIsActive)
        {
            SetConfigurationUiEnabled(false);
            UpdateStreamToolbarVisibility();
        }
        ApplyDisplayOrder(origin, LogSessionViewForSession(origin));
    }
    if (originIsActive)
    {
        UpdateStreamingStatus();
        UpdateWindowTitle();
    }

    auto fileSource = std::make_unique<loglib::FileLineSource>(std::move(logFile));
    loglib::FileLineSource *fileSourcePtr = fileSource.get();
    QtStreamingLogSink *sink = model->Sink();

    loglib::ParserOptions options;
    options.configuration = std::move(cfg);

    // Per-file parser detection runs against the effective (possibly
    // decompressed) path so the sniff sees the actual bytes.
    // `origin->MutableCurrentSource()` still stores the first file's session-level format.
    const DetectedFormat detectedPerFile = DetectFormatForPath(effectivePath);
    std::shared_ptr<loglib::LogParser> parser =
        MakeParserForFormat(detectedPerFile.format, detectedPerFile.regexPattern);

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
        model->BeginStreaming(std::move(fileSource), std::move(parseCallable));
    }
    else
    {
        model->AppendStreaming(std::move(fileSource), std::move(parseCallable));
    }
    // Parse worker armed; caller unwinds and awaits `streamingFinished`.
    return true;
}

void MainWindow::BeginAsyncDecompression(
    LogSession *origin, const QString &originalPath, loglib::internal::DecompressingByteSource::Codec codec
)
{
    if (origin == nullptr)
    {
        return;
    }
    // Fresh stop-source per open so a leftover cancel from a prior
    // run can't leak into this one.
    origin->MutableDecompressionStopSource() = loglib::StopSource{};
    origin->MutableDecompressionBytesIn().storeRelaxed(0);
    // Up-front compressed size lets the poll timer compute a
    // percentage without waiting for the worker's first tick.
    std::error_code sizeEc;
    // Use `QStringToFsPath` -- on Windows `path(std::string)` decodes
    // as ACP so non-ASCII filenames would produce a "not found" error.
    // MSVC's <filesystem> flag-cast trips clang-analyzer's enum-cast check.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto compressedSize = std::filesystem::file_size(logapp::QStringToFsPath(originalPath), sizeEc);
    origin->MutableDecompressionTotalBytesIn().storeRelaxed(sizeEc ? 0 : static_cast<qint64>(compressedSize));

    origin->SetDecompressionOriginalPath(originalPath);
    // Pass the string_view size explicitly: `CodecName` currently
    // returns views over string literals, but NUL-termination is
    // not part of the string_view contract.
    const std::string_view codecName = loglib::internal::CodecName(codec);
    origin->SetDecompressionCodecName(QString::fromLatin1(codecName.data(), static_cast<qsizetype>(codecName.size())));
    origin->SetDecompressionStartedAt(std::chrono::steady_clock::now());
    // See `LogSession::IsDecompressionInFlight`: guards the
    // finished slot against stale callout events dispatched between
    // a future completing and a subsequent cancel. The rising edge
    // also bumps `DecompressionGeneration()`; capture it below so
    // the poll timer's re-entry guard can distinguish "still the
    // same op" from "queued completion silently rearmed for the
    // next file".
    origin->SetDecompressionInFlight(true);

    LogSessionView *originView = LogSessionViewForSession(origin);
    if (originView != nullptr)
    {
        originView->SetContentEnabled(false);
    }

    ShowDecompressionProgress();
    // Show the "Preparing…" frame up-front so the dialog isn't
    // blank while the first worker chunk lands.
    if (mDecompressionProgressDialog)
    {
        mDecompressionProgressDialog->setLabelText(
            tr("Decompressing %1 (%2)\nPreparing…")
                .arg(QFileInfo(originalPath).fileName(), origin->DecompressionCodecName())
        );
    }

    // Worker runs the full synchronous decode. Wrapped in a shared_ptr
    // so both the future and any downstream `parseCallable` that pins
    // the temp file can share ownership. All captures are owned by
    // value: no MainWindow reference escapes.
    const auto stopToken = origin->DecompressionStopSource().get_token();
    auto *sharedBytesIn = &origin->MutableDecompressionBytesIn();
    auto *sharedTotal = &origin->MutableDecompressionTotalBytesIn();

    // Bundle metadata stripping requires both the extension and zstd.
    const bool isSessionBundle =
        IsSessionBundlePath(originalPath) && codec == loglib::internal::DecompressingByteSource::Codec::Zstd;
    // Convert on the GUI thread via `QStringToFsPath` so non-ASCII
    // bundle names survive the hop into the worker (see the
    // `file_size` note above).
    const std::filesystem::path input = logapp::QStringToFsPath(originalPath);
    // `clang-analyzer-webkit.UncountedLambdaCapturesChecker` is WebKit-specific
    // and misclassifies `QAtomicInteger *` captures -- they are `this` members
    // guarded by `mSession->IsDecompressionInFlight()`. `bugprone-exception-escape` is a
    // false positive on `QtConcurrent::run`, which stores any escaped
    // exception into the returned `QFuture`.
    // NOLINTNEXTLINE(clang-analyzer-webkit.UncountedLambdaCapturesChecker,bugprone-exception-escape)
    auto future = QtConcurrent::run([input, sharedBytesIn, sharedTotal, stopToken, isSessionBundle]() {
        // NOLINTNEXTLINE(clang-analyzer-webkit.UncountedLambdaCapturesChecker)
        auto progressCb = [sharedBytesIn, sharedTotal](const loglib::internal::DecompressingByteSource::Progress &p) {
            // Relaxed: the GUI only needs a recent-enough snapshot.
            sharedBytesIn->storeRelaxed(static_cast<qint64>(p.bytesIn));
            sharedTotal->storeRelaxed(static_cast<qint64>(p.totalBytesIn));
        };
        loglib::internal::DecompressingByteSource::Options options;
        options.discardFirstLine = isSessionBundle;
        return std::make_shared<loglib::internal::DecompressingByteSource>(
            input, std::move(progressCb), stopToken, options
        );
    });

    // Own our own watcher: `LogModel::mStreamingWatcher` asserts
    // its own future is idle before a parse job, so reusing it
    // would trip that assertion on the parse hand-off. The watcher
    // itself is session-scoped so
    // tab/session teardown reaps it automatically; the connection
    // stays on `this` because the slot body operates on widgets.
    // `Qt::UniqueConnection` keeps this safe against repeated
    // `Begin*` calls -- `EnsureDecompressionWatcher` returns the
    // cached instance on the second call but the `finished` slot
    // is only invoked once per completion.
    auto *watcher = origin->EnsureDecompressionWatcher();
    connect(
        watcher,
        &LogSession::DecompressionWatcher::finished,
        this,
        &MainWindow::OnDecompressionFinished,
        Qt::UniqueConnection
    );
    watcher->setFuture(future);
}

void MainWindow::ShowDecompressionProgress()
{
    if (!mSuppressDialogsForTest)
    {
        if (!mDecompressionProgressDialog)
        {
            mDecompressionProgressDialog = new QProgressDialog(this);
            mDecompressionProgressDialog->setWindowTitle(tr("Decompressing"));
            mDecompressionProgressDialog->setWindowModality(Qt::NonModal);
            mDecompressionProgressDialog->setMinimumDuration(DECOMPRESSION_DIALOG_DEFER_MS);
            mDecompressionProgressDialog->setRange(0, PROGRESS_PERCENT_MAX);
            mDecompressionProgressDialog->setAutoClose(false);
            mDecompressionProgressDialog->setAutoReset(false);
            connect(mDecompressionProgressDialog.data(), &QProgressDialog::canceled, this, [this]() {
                if (mSession != nullptr && mSession->IsDecompressionInFlight())
                {
                    mSession->MutableDecompressionStopSource().request_stop();
                }
            });
        }
    }

    if (mDecompressionPollTimer == nullptr)
    {
        mDecompressionPollTimer = new QTimer(this);
        mDecompressionPollTimer->setInterval(DECOMPRESSION_POLL_INTERVAL_MS);
        connect(mDecompressionPollTimer, &QTimer::timeout, this, &MainWindow::UpdateDecompressionProgressUi);
    }
    mDecompressionPollTimer->start();
    UpdateDecompressionProgressUi();
}

void MainWindow::UpdateDecompressionProgressUi()
{
    bool anyInFlight = false;
    for (LogSession *session : hostedSessions())
    {
        if (session == nullptr || !session->IsDecompressionInFlight())
        {
            continue;
        }
        anyInFlight = true;
        LogSessionView *view = LogSessionViewForSession(session);
        if (view == nullptr)
        {
            continue;
        }
        const qint64 bytesIn = session->DecompressionBytesIn().loadRelaxed();
        const qint64 total = session->DecompressionTotalBytesIn().loadRelaxed();
        const QString displayName = QFileInfo(session->DecompressionOriginalPath()).fileName();
        const QString header = session->DecompressionCodecName().isEmpty()
                                   ? tr("Decompressing %1").arg(displayName)
                                   : tr("Decompressing %1 (%2)").arg(displayName, session->DecompressionCodecName());
        int pct = -1;
        QString label;
        if (total > 0)
        {
            pct = static_cast<int>((PROGRESS_PERCENT_MAX * bytesIn) / total);
            pct = std::min(pct, PROGRESS_PERCENT_MAX);
            label = tr("%1\n%2 / %3 compressed bytes read")
                        .arg(
                            header,
                            HumanBytes(static_cast<std::size_t>(bytesIn)),
                            HumanBytes(static_cast<std::size_t>(total))
                        );
        }
        else
        {
            label = tr("%1\nPreparing\u2026").arg(header);
        }
        if (view->IsOperationProgressVisible())
        {
            view->UpdateOperationProgress(label, pct);
        }
        else
        {
            view->ShowOperationProgress(label, pct);
        }
    }

    if (!anyInFlight)
    {
        TeardownDecompressionProgress();
        return;
    }

    if (mSuppressDialogsForTest || !mDecompressionProgressDialog)
    {
        return;
    }

    if (mSession == nullptr || !mSession->IsDecompressionInFlight())
    {
        mDecompressionProgressDialog->hide();
        return;
    }

    const qint64 bytesIn = mSession->DecompressionBytesIn().loadRelaxed();
    const qint64 total = mSession->DecompressionTotalBytesIn().loadRelaxed();
    const std::uint64_t generation = mSession->DecompressionGeneration();
    const QString displayName = QFileInfo(mSession->DecompressionOriginalPath()).fileName();
    const QString header = mSession->DecompressionCodecName().isEmpty()
                               ? tr("Decompressing %1").arg(displayName)
                               : tr("Decompressing %1 (%2)").arg(displayName, mSession->DecompressionCodecName());
    if (total > 0)
    {
        const int pct = static_cast<int>((PROGRESS_PERCENT_MAX * bytesIn) / total);
        mDecompressionProgressDialog->setValue(std::min(pct, PROGRESS_PERCENT_MAX));
        if (mSession == nullptr || !mSession->IsDecompressionInFlight() ||
            mSession->DecompressionGeneration() != generation || !mDecompressionProgressDialog)
        {
            return;
        }
        mDecompressionProgressDialog->setLabelText(
            tr("%1\n%2 / %3 compressed bytes read")
                .arg(header, HumanBytes(static_cast<std::size_t>(bytesIn)), HumanBytes(static_cast<std::size_t>(total)))
        );
    }
    else
    {
        mDecompressionProgressDialog->setLabelText(tr("%1\nPreparing\u2026").arg(header));
    }
}

void MainWindow::TeardownDecompressionProgress()
{
    if (mDecompressionPollTimer != nullptr)
    {
        mDecompressionPollTimer->stop();
    }
    if (mDecompressionProgressDialog)
    {
        mDecompressionProgressDialog->reset();
        mDecompressionProgressDialog->hide();
    }
}

void MainWindow::CancelInFlightDecompression()
{
    CancelInFlightDecompressionFor(mSession);
}

void MainWindow::CancelInFlightDecompressionFor(LogSession *origin)
{
    if (origin == nullptr)
    {
        return;
    }
    const bool wasInFlight = origin->IsDecompressionInFlight();
    QPointer<LogSessionView> originView = LogSessionViewForSession(origin);
    origin->SetDecompressionInFlight(false);
    if (wasInFlight)
    {
        try
        {
            origin->ClearApplyEmbeddedBundleConfig();
        }
        catch (const std::exception &) // NOLINT(bugprone-empty-catch)
        {
            // The session is closing or the window is being destroyed.
        }
    }

    auto *watcher = origin->DecompressionWatcherPtr();
    if (watcher == nullptr)
    {
        origin->ClearDecompressionScratchPaths();
        origin->MutablePendingDecompressionErrors().clear();
        if (originView != nullptr)
        {
            originView->HideOperationProgress();
            if (wasInFlight)
            {
                originView->SetContentEnabled(true);
            }
        }
        UpdateDecompressionProgressUi();
        return;
    }

    origin->MutableDecompressionStopSource().request_stop();
    origin->DrainDecompressionWatcher();
    origin->ClearDecompressionScratchPaths();
    origin->MutablePendingDecompressionErrors().clear();
    if (originView != nullptr)
    {
        originView->HideOperationProgress();
        if (wasInFlight)
        {
            originView->SetContentEnabled(true);
        }
    }
    UpdateDecompressionProgressUi();
}

void MainWindow::OnDecompressionFinished()
{
    // Resolve origin from watcher ownership. A non-null sender that is not in
    // the hosted registry is an unhosted completion: drain nothing here (close
    // already cancelled the worker) and do not fall back to the active tab.
    LogSession *origin = LogSessionForDecompressionWatcher(sender());
    if (origin == nullptr)
    {
        if (sender() != nullptr)
        {
            return;
        }
        origin = mSession;
    }
    if (mOperations == nullptr)
    {
        return;
    }
    mOperations->CompleteDecompression(origin);
}

void MainWindow::OnDecompressionFinishedFor(LogSession *origin)
{
    if (origin == nullptr)
    {
        return;
    }
    LogModel *const model = origin->Model();
    AnchorManager *const anchors = origin->Anchors();
    HighlightRuleSet *const highlights = origin->Highlights();
    LogSessionView *originView = LogSessionViewForSession(origin);

    // Bail if the cancel path already ran (see
    // `LogSession::IsDecompressionInFlight` for why this can happen even
    // after `setFuture({})`). Reading `result()` off the reset
    // future here would splice a bogus "Failed to open ''" into
    // the freshly-armed session.
    if (!origin->IsDecompressionInFlight())
    {
        return;
    }
    origin->SetDecompressionInFlight(false);

    if (originView != nullptr)
    {
        originView->HideOperationProgress();
        originView->SetContentEnabled(true);
    }
    UpdateDecompressionProgressUi();

    auto *watcher = origin->DecompressionWatcherPtr();
    if (watcher == nullptr)
    {
        // Defensive: `finished` should never fire without a
        // watcher, but bail cleanly if teardown raced us.
        return;
    }

    std::shared_ptr<loglib::internal::DecompressingByteSource> dbs;
    std::optional<loglib::SessionBundleMetadata> bundleMetadata;
    QString errorEntry;
    bool cancelled = false;
    try
    {
        dbs = watcher->result();
    }
    catch (const loglib::internal::DecompressionCancelled &)
    {
        cancelled = true;
    }
    catch (const std::exception &e)
    {
        errorEntry = tr("Failed to decompress '%1': %2")
                         .arg(origin->DecompressionOriginalPath(), QString::fromLocal8Bit(e.what()));
    }
    catch (...)
    {
        errorEntry = tr("Failed to decompress '%1': unknown error").arg(origin->DecompressionOriginalPath());
    }
    // Report malformed metadata as a bundle error, not a codec error.
    if (errorEntry.isEmpty() && !cancelled && dbs && IsSessionBundlePath(origin->DecompressionOriginalPath()))
    {
        try
        {
            bundleMetadata.emplace(loglib::ParseSessionBundleMetadata(dbs->DiscardedFirstLine()));
        }
        catch (const loglib::SessionBundleVersionError &e)
        {
            errorEntry = tr("Cannot open session bundle '%1': %2")
                             .arg(origin->DecompressionOriginalPath(), QString::fromLocal8Bit(e.what()));
        }
        catch (const loglib::SessionBundleReadError &e)
        {
            errorEntry = tr("'%1' is not a valid session bundle: %2")
                             .arg(origin->DecompressionOriginalPath(), QString::fromLocal8Bit(e.what()));
        }
    }

    // Detach the future so a follow-up decompression re-arms
    // cleanly; ownership of the shared_ptr has already moved to `dbs`.
    watcher->setFuture(QFuture<LogSession::DecompressionByteSourcePtr>{});

    if (cancelled)
    {
        origin->ClearApplyEmbeddedBundleConfig();
        // User cancels surface as a status-bar toast, not a modal.
        // The queue drain continues so the rest of the batch still
        // opens. Background origins queue the toast until selected.
        PostStatusMessage(
            origin,
            tr("Decompression cancelled: %1").arg(QFileInfo(origin->DecompressionOriginalPath()).fileName()),
            STATUS_BAR_MESSAGE_TIMEOUT_MS
        );
        origin->ClearDecompressionScratchPaths();
        StreamNextPendingFile(origin);
        // Chain-terminal cancel: drain accumulated errors and
        // return the session UI to Idle. No-op if a successor
        // worker was armed.
        FinalizeAfterDecompressionIfChainTerminal(origin);
        return;
    }

    if (!errorEntry.isEmpty())
    {
        origin->ClearApplyEmbeddedBundleConfig();
        origin->MutablePendingDecompressionErrors().push_back(errorEntry.toStdString());
        // Clear scratch fields BEFORE draining -- a compressed
        // follow-up would re-enter `BeginAsyncDecompression` and
        // repopulate them; clearing after that would wipe the new
        // file's metadata. The success branch below doesn't need
        // the pre-clear because a parse worker is armed and the
        // follow-up waits for `OnStreamingFinished`.
        origin->ClearDecompressionScratchPaths();
        StreamNextPendingFile(origin);
        // Chain-terminal decompression failure: drains
        // `origin->MutablePendingDecompressionErrors()` under the `Error
        // Decompressing File` title. Without this call the batch
        // (including THIS entry) would sit until the next
        // destructive session boundary silently cleared it.
        FinalizeAfterDecompressionIfChainTerminal(origin);
        return;
    }

    // Apply only the latest armed bundle to a still-empty session.
    const bool armedForThisFile = origin->ShouldApplyEmbeddedBundleConfig() &&
                                  origin->ApplyEmbeddedBundleConfigForPath() == origin->DecompressionOriginalPath();
    const bool sessionStillFresh = !origin->MutableCurrentSource().has_value();
    if (bundleMetadata.has_value() && armedForThisFile && sessionStillFresh && model != nullptr)
    {
        loglib::LogConfiguration embedded = std::move(bundleMetadata->configuration);
        const std::string displayPath = logapp::CanonicalDisplayPath(origin->DecompressionOriginalPath()).toStdString();
        const std::string dedupKey = logapp::CanonicalLocator(origin->DecompressionOriginalPath()).toStdString();
        if (!embedded.source.has_value())
        {
            embedded.source.emplace();
        }
        embedded.source->kind = loglib::LogConfiguration::Source::Kind::File;
        loglib::ClearLocators(*embedded.source);
        loglib::AppendLocator(*embedded.source, displayPath, dedupKey);
        // Anchors use the same canonical locator as source deduplication.
        for (auto &anchor : embedded.anchors)
        {
            anchor.locator = dedupKey;
        }

        model->ConfigurationManager().SetConfiguration(std::move(embedded));
        model->NotifyConfigurationReplaced();
        origin->SetPendingApplySortFromConfig(true);
        if (anchors != nullptr)
        {
            // Preserve newer anchors by clamping unsupported colour slots.
            const std::size_t clampedAnchorCount = anchors->Replace(model->Configuration().anchors);
            if (clampedAnchorCount > 0)
            {
                PostStatusMessage(
                    origin,
                    tr("%1 anchor(s) from a newer schema had their colour clamped to slot %2.")
                        .arg(static_cast<qulonglong>(clampedAnchorCount))
                        .arg(static_cast<qulonglong>(loglib::ANCHOR_PALETTE_SIZE)),
                    STATUS_BAR_MESSAGE_TIMEOUT_MS
                );
            }
        }
        RebuildFiltersFromConfiguration(origin);

        // Rebuild the runtime highlight cache from embedded rules.
        if (highlights != nullptr)
        {
            const auto &appliedConfig = model->Configuration();
            highlights->SetRules(appliedConfig.highlightRules, appliedConfig.columns, &model->Table());
            const std::size_t inactive = highlights->InactiveCount();
            if (inactive > 0)
            {
                PostStatusMessage(
                    origin,
                    tr("%1 highlight rule(s) inactive against the loaded columns.")
                        .arg(static_cast<qulonglong>(inactive)),
                    STATUS_BAR_MESSAGE_TIMEOUT_MS
                );
            }
            // Only sync the editor when it is bound to the origin session.
            if (mHighlightRulesEditor != nullptr && mHighlightRulesEditorSession.data() == origin)
            {
                mHighlightRulesEditor->SetColumns(appliedConfig.columns);
                mHighlightRulesEditor->SetRules(appliedConfig.highlightRules);
            }
            origin->ClearHighlightEditorDraft();
        }
    }
    // This bundle has consumed its intent even if the session was busy.
    if (bundleMetadata.has_value() && armedForThisFile)
    {
        origin->ClearApplyEmbeddedBundleConfig();
    }

    // Success. Emit the "Decompressed X -> Y in Zs" toast before
    // the parse-status label overwrites it; the timeout gives a
    // 5 s window that survives a fast parse start.
    if (dbs)
    {
        const auto elapsed = std::chrono::steady_clock::now() - origin->DecompressionStartedAt();
        // Explicit size (see the matching site in `BeginAsyncDecompression`).
        const std::string_view codecName = loglib::internal::CodecName(dbs->DetectedCodec());
        const QString msg = tr("Decompressed %1 (%2 \u2192 %3, %4) in %5")
                                .arg(
                                    QFileInfo(origin->DecompressionOriginalPath()).fileName(),
                                    HumanBytes(dbs->CompressedSize()),
                                    HumanBytes(dbs->DecompressedSize()),
                                    QString::fromLatin1(codecName.data(), static_cast<qsizetype>(codecName.size())),
                                    HumanDuration(elapsed)
                                );
        PostStatusMessage(origin, msg, STATUS_BAR_MESSAGE_TIMEOUT_MS);
    }

    // Resolve before moving `dbs`; argument evaluation order is not fixed.
    const std::filesystem::path effectivePath =
        dbs ? dbs->EffectivePath() : logapp::QStringToFsPath(origin->DecompressionOriginalPath());

    // Transfer temp-file ownership to the mapped `LogFile`.
    const bool armedParseWorker =
        ContinueOpenAfterPrepared(origin, origin->DecompressionOriginalPath(), effectivePath, std::move(dbs));
    // Clear scratch fields BEFORE the follow-up drain (see the
    // error branch above for the ordering rationale).
    origin->ClearDecompressionScratchPaths();
    if (!armedParseWorker)
    {
        StreamNextPendingFile(origin);
        FinalizeAfterDecompressionIfChainTerminal(origin);
    }
}

std::vector<int> MainWindow::CollectExportSourceRows(bool selectionOnly) const
{
    if (mModel == nullptr || mSortFilterProxyModel == nullptr)
    {
        return {};
    }

    const int filteredCount = mSortFilterProxyModel->rowCount();
    if (filteredCount <= 0)
    {
        return {};
    }

    // `[outer proxy row] -> keep?` flag map for the display-order
    // walk below. Empty means "no selection filter"; non-empty
    // restricts to the selected outer rows.
    std::vector<bool> selectedOuterRows;
    if (selectionOnly && mTableView != nullptr && mTableView->selectionModel() != nullptr)
    {
        const QModelIndexList selected = mTableView->selectionModel()->selectedRows();
        // Indexes are minted against `mSortFilterProxyModel`
        // (the outermost proxy the view sees).
        selectedOuterRows.assign(static_cast<std::size_t>(filteredCount), false);
        for (const QModelIndex &idx : selected)
        {
            if (idx.model() != mSortFilterProxyModel)
            {
                continue;
            }
            const int row = idx.row();
            if (row >= 0 && static_cast<std::size_t>(row) < selectedOuterRows.size())
            {
                selectedOuterRows[static_cast<std::size_t>(row)] = true;
            }
        }
    }

    // Walk the outer proxy in display order and resolve each row
    // down through the full proxy chain (`view ->
    // SortFilterProxyModel -> RowOrderProxyModel -> LogModel`).
    // Iterating the outermost proxy is what pins the export to
    // the user's visible order, including the newest-first flip
    // -- a single `mapToSource` would silently drop that layer.
    std::vector<int> sourceRows;
    sourceRows.reserve(static_cast<std::size_t>(filteredCount));
    for (int outerRow = 0; outerRow < filteredCount; ++outerRow)
    {
        if (!selectedOuterRows.empty() && !selectedOuterRows[static_cast<std::size_t>(outerRow)])
        {
            continue;
        }
        QModelIndex walker = mSortFilterProxyModel->index(outerRow, 0);
        while (walker.isValid())
        {
            const auto *proxy = qobject_cast<const QAbstractProxyModel *>(walker.model());
            if (proxy == nullptr)
            {
                break;
            }
            walker = proxy->mapToSource(walker);
        }
        if (walker.isValid() && walker.row() >= 0)
        {
            sourceRows.push_back(walker.row());
        }
    }
    return sourceRows;
}

void MainWindow::ExportFilteredRows()
{
    if (mModel == nullptr || mSortFilterProxyModel == nullptr)
    {
        return;
    }
    if (mModel->rowCount() == 0)
    {
        QMessageBox::information(this, tr("Export Filtered Rows"), tr("No rows are currently loaded."));
        return;
    }

    // No overlapping exports: the shortcut can fire during the
    // < 500 ms window before the progress dialog appears.
    if (mSession->IsExportInFlight())
    {
        return;
    }

    // The worker reads `LogTable` from a background thread with
    // no lock, so refuse to start while the model is being mutated
    // by a live stream / bulk load. Bulk loads finish quickly
    // (retry after `streamingFinished`); live-tail needs Stop
    // (not Pause -- Pause leaves the producer streaming into a
    // buffer that would flush on resume and race the export).
    if (mModel->IsStreamingActive() || mSession->IsDecompressionInFlight())
    {
        const QString detail = IsLiveTailSession() ? tr("Stop the live-tail session (Ctrl+Shift+X) before exporting.")
                                                   : tr("Wait for the current file load to finish, then retry.");
        QMessageBox::information(this, tr("Export Filtered Rows"), detail);
        return;
    }

    // Snapshot proxy row order + visible columns up-front so we
    // can hand a self-owned plan to the worker without touching
    // Qt models from a background thread.
    const int filteredCount = mSortFilterProxyModel->rowCount();

    std::size_t selectionCount = 0;
    if (mTableView != nullptr && mTableView->selectionModel() != nullptr)
    {
        const QModelIndexList selected = mTableView->selectionModel()->selectedRows();
        selectionCount = static_cast<std::size_t>(selected.size());
    }

    // Default filename stem: source basename if we have one, else "export".
    QString defaultStem = QStringLiteral("export");
    if (const auto &currentSource = mSession->CurrentSource();
        currentSource.has_value() && !currentSource->locators.empty())
    {
        const QFileInfo info(QString::fromStdString(currentSource->locators.front()));
        const QString base = info.completeBaseName();
        if (!base.isEmpty())
        {
            defaultStem = base;
        }
    }

    ExportDialog dialog(
        static_cast<std::size_t>(filteredCount),
        selectionCount,
        defaultStem,
        DefaultExportDir(),
        IsLiveTailSession(),
        this
    );
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    const auto config = dialog.Configuration();
    if (config.destination.isEmpty())
    {
        return;
    }
    // `RememberLastExportDir` is deferred to the success branch of
    // `OnExportFinished` so a failed export (bad path, perms, disk
    // full) does not stick as the remembered directory.

    std::vector<int> sourceRows = CollectExportSourceRows(config.selectionOnly);

    if (sourceRows.empty())
    {
        QMessageBox::information(this, tr("Export Filtered Rows"), tr("No rows match the current selection."));
        return;
    }

    // CSV / Markdown honour `includeHiddenColumns`; JSON /
    // Snapshot are row-shape and ignore this vector.
    std::vector<std::size_t> visibleColumns;
    const auto &configuration = mModel->Configuration();
    visibleColumns.reserve(configuration.columns.size());
    for (std::size_t i = 0; i < configuration.columns.size(); ++i)
    {
        if (config.includeHiddenColumns || configuration.columns[i].visible)
        {
            visibleColumns.push_back(i);
        }
    }

    auto plan = std::make_unique<slv::exports::ExportPlan>();
    plan->format = config.format;
    plan->sourceRows = std::move(sourceRows);
    plan->visibleColumns = std::move(visibleColumns);
    plan->includeAllFieldsForJson = true; // v1: JSON always includes every field.
    plan->includeHeaderRow = config.includeHeaderRow;
    plan->table = &mModel->Table();
    // Preserve non-ASCII filenames on Windows -- see `qstring_path.hpp`.
    plan->destination = logapp::QStringToFsPath(config.destination);

    const QString formatLabel = QString::fromLatin1(slv::exports::LabelFor(config.format));
    BeginAsyncExport(std::move(plan), config.destination, formatLabel);
}

void MainWindow::BeginAsyncExport(
    std::unique_ptr<slv::exports::ExportPlan> plan, const QString &destination, const QString &formatLabel
)
{
    // Fresh per-run stop source so a leftover cancel from a prior
    // export cannot bleed into this one.
    mSession->MutableExportStopSource() = loglib::StopSource{};
    mSession->MutableExportRowsWritten().storeRelaxed(0);
    mSession->MutableExportRowsTotal().storeRelaxed(static_cast<qint64>(plan->sourceRows.size()));

    mSession->SetExportDestinationPath(destination);
    mSession->SetExportFormatLabel(formatLabel);
    mSession->SetExportStartedAt(std::chrono::steady_clock::now());
    mSession->SetExportInFlight(true);

    LogSessionView *originView = LogSessionViewForSession(mSession);
    if (originView != nullptr)
    {
        originView->SetContentEnabled(false);
    }

    ShowExportProgress();
    if (mExportProgressDialog)
    {
        // `%L1` for locale-grouped digits; `qulonglong` avoids
        // narrowing where `size_t` is 32-bit.
        mExportProgressDialog->setLabelText(tr("Exporting %L1 rows to %2\nPreparing\u2026")
                                                .arg(static_cast<qulonglong>(plan->sourceRows.size()))
                                                .arg(QFileInfo(destination).fileName()));
    }

    // Share the plan + sink so the worker capture cannot race the
    // finished-slot re-reading its members. Build the sink up-front
    // so open failures surface synchronously.
    const std::shared_ptr<slv::exports::ExportPlan> sharedPlan(std::move(plan));
    std::shared_ptr<slv::exports::FileSink> sink;
    try
    {
        sink = std::make_shared<slv::exports::FileSink>(sharedPlan->destination);
    }
    catch (const std::exception &e)
    {
        // Open failed synchronously: tear down and toast.
        mSession->SetExportInFlight(false);
        QPointer<LogSessionView> originViewToReEnable = LogSessionViewForSession(mSession);
        if (originViewToReEnable != nullptr)
        {
            originViewToReEnable->HideOperationProgress();
            originViewToReEnable->SetContentEnabled(true);
        }
        UpdateExportProgressUi();
        QMessageBox::warning(
            this,
            tr("Export Failed"),
            tr("Failed to open '%1' for writing: %2").arg(destination, QString::fromLocal8Bit(e.what()))
        );
        return;
    }

    const auto stopToken = mSession->ExportStopSource().get_token();
    auto *rowsWrittenAtomic = &mSession->MutableExportRowsWritten();

    // Worker: drive the exporter into the sink synchronously.
    // Throws propagate out of `QtConcurrent::run` and land on
    // `waitForFinished()` in the finished slot; the progress
    // callback publishes into the GUI atomic that the poll timer
    // reads on the next tick.
    // NOLINTNEXTLINE(clang-analyzer-webkit.UncountedLambdaCapturesChecker)
    auto future = QtConcurrent::run([sharedPlan, sink, stopToken, rowsWrittenAtomic]() {
        auto exporter = slv::exports::MakeExporter(sharedPlan->format);
        if (exporter == nullptr)
        {
            throw std::runtime_error("Unsupported export format");
        }
        // Free-function callback: keeps the capture list empty
        // and matches the C-style ProgressCallback signature.
        auto progressCb = +[](void *userData, size_t rowsWritten, size_t /*total*/) {
            auto *dst = static_cast<QAtomicInteger<qint64> *>(userData);
            dst->storeRelaxed(static_cast<qint64>(rowsWritten));
        };
        exporter->Run(sharedPlan->View(), *sink, stopToken, progressCb, rowsWrittenAtomic);
        // `RowExporter::Run` never calls `Finish`, so a mid-run
        // throw unwinds through `~FileSink` and unlinks the temp
        // file. Success path is exactly one `Finish` after `Run`
        // returns cleanly.
        sink->Finish();
        rowsWrittenAtomic->storeRelaxed(static_cast<qint64>(sharedPlan->sourceRows.size()));
    });

    auto *watcher = mSession->EnsureExportWatcher();
    connect(watcher, &LogSession::ExportWatcher::finished, this, &MainWindow::OnExportFinished, Qt::UniqueConnection);
    mSession->SetExportIsBundle(false);
    watcher->setFuture(future);
}

void MainWindow::ExportSessionBundle()
{
    if (mModel->rowCount() == 0)
    {
        QMessageBox::information(this, tr("Export Session Bundle"), tr("No rows are currently loaded."));
        return;
    }
    if (mSession->IsExportInFlight())
    {
        return;
    }
    if (mSession->IsDecompressionInFlight())
    {
        // A load pass is still in-flight; there is nothing coherent
        // to snapshot yet. Ask the user to wait rather than force a
        // partial export.
        QMessageBox::information(
            this, tr("Export Session Bundle"), tr("Wait for the current file load to finish, then retry.")
        );
        return;
    }
    if (mModel->IsStreamingActive())
    {
        if (!IsLiveTailSession())
        {
            // A one-shot file must finish loading before export.
            QMessageBox::information(
                this, tr("Export Session Bundle"), tr("Wait for the current file load to finish, then retry.")
            );
            return;
        }
        // Stop the producer so queued batches cannot mutate the table.
#ifdef LOGAPP_BUILD_TESTING
        if (!mSuppressDialogsForTest)
#endif
        {
            const auto response = QMessageBox::question(
                this,
                tr("Export Session Bundle"),
                tr("Exporting a bundle will stop the live-tail session. Existing rows are captured "
                   "in the bundle, but new lines from the tailed source are not preserved. Continue?"),
                QMessageBox::Ok | QMessageBox::Cancel,
                QMessageBox::Ok
            );
            if (response != QMessageBox::Ok)
            {
                return;
            }
        }
        StopStream();
    }
    // Export all retained rows; the filter is preserved as view state.
    const std::size_t rowCount = mModel->Table().Data().Lines().size();

    QString defaultStem;
    if (const auto &currentSource = mSession->CurrentSource();
        currentSource.has_value() && !currentSource->locators.empty())
    {
        const QString primary = QString::fromStdString(currentSource->locators.front());
        const QFileInfo info(primary);
        defaultStem = info.completeBaseName();
        // Avoid repeating the extension for names such as
        // `foo.slvbundle.slvbundle`.
        if (IsSessionBundlePath(primary) &&
            defaultStem.endsWith(QLatin1String(loglib::SESSION_BUNDLE_EXTENSION), Qt::CaseInsensitive))
        {
            defaultStem.chop(QLatin1String(loglib::SESSION_BUNDLE_EXTENSION).size());
        }
    }
    if (defaultStem.isEmpty())
    {
        // Give non-file sources a collision-resistant default name.
        defaultStem =
            QStringLiteral("session-%1").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-hhmmss")));
    }

    SessionBundleDialog dialog(rowCount, defaultStem, DefaultExportDir(), IsLiveTailSession(), this);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    const auto config = dialog.Configuration();
    if (config.destination.isEmpty())
    {
        return;
    }

    // Copy current view state into the embedded configuration.
    MirrorSessionStateToConfiguration();

    BeginAsyncBundleExport(logapp::QStringToFsPath(config.destination), config.compressionLevel, config.totalWorkers);
}

void MainWindow::ExportSessionBundleToPathForTest(const QString &destination)
{
    // Run production preflight without showing the dialog.
    if (destination.isEmpty())
    {
        return;
    }
    if (mModel->rowCount() == 0)
    {
        return;
    }
    if (mSession->IsExportInFlight() || mSession->IsDecompressionInFlight())
    {
        return;
    }
    if (mModel->IsStreamingActive())
    {
        if (!IsLiveTailSession())
        {
            return;
        }
        StopStream();
    }
    MirrorSessionStateToConfiguration();
    constexpr int DEFAULT_TEST_COMPRESSION_LEVEL = 3;
    constexpr int DEFAULT_TEST_TOTAL_WORKERS = 0;
    BeginAsyncBundleExport(
        logapp::QStringToFsPath(destination), DEFAULT_TEST_COMPRESSION_LEVEL, DEFAULT_TEST_TOTAL_WORKERS
    );
}

void MainWindow::BeginAsyncBundleExport(std::filesystem::path destination, int compressionLevel, int totalWorkers)
{
    mSession->MutableExportStopSource() = loglib::StopSource{};
    mSession->MutableExportRowsWritten().storeRelaxed(0);
    mSession->MutableExportRowsTotal().storeRelaxed(static_cast<qint64>(mModel->Table().Data().Lines().size()));

    // Preserve non-ASCII destination names on Windows.
    mSession->SetExportDestinationPath(logapp::FsPathToQString(destination));
    mSession->SetExportFormatLabel(tr("Session bundle"));
    mSession->SetExportStartedAt(std::chrono::steady_clock::now());
    mSession->SetExportInFlight(true);
    mSession->SetExportIsBundle(true);

    LogSessionView *originView = LogSessionViewForSession(mSession);
    if (originView != nullptr)
    {
        originView->SetContentEnabled(false);
    }

    ShowExportProgress();
    if (mExportProgressDialog)
    {
        mExportProgressDialog->setLabelText(tr("Exporting %L1 rows into %2\nPreparing\u2026")
                                                .arg(static_cast<qulonglong>(mModel->Table().Data().Lines().size()))
                                                .arg(QFileInfo(mSession->ExportDestinationPath()).fileName()));
    }

    // Snapshot on the GUI thread, including runtime-only anchors that
    // the writer remaps into the bundle's dense row space.
    loglib::LogConfiguration configSnapshot = mModel->Configuration();
    if (mAnchors != nullptr)
    {
        configSnapshot.anchors = mAnchors->EntriesIncludingRuntimeOnly();
    }
    const loglib::LogTable *const tablePtr = &mModel->Table();

    const auto stopToken = mSession->ExportStopSource().get_token();
    auto *rowsWrittenAtomic = &mSession->MutableExportRowsWritten();

    // Let worker exceptions propagate through the future.
    // The WebKit lambda-captures checker misclassifies `QAtomicInteger *`
    // captures; `rowsWrittenAtomic` is a `this` member outliving the job.
    // NOLINTBEGIN(clang-analyzer-webkit.UncountedLambdaCapturesChecker)
    auto future = QtConcurrent::run([tablePtr,
                                     configSnapshot,
                                     destination = std::move(destination),
                                     stopToken,
                                     rowsWrittenAtomic,
                                     compressionLevel,
                                     totalWorkers]() {
        loglib::SessionBundleWriteOptions options;
        options.compressionLevel = compressionLevel;
        options.totalWorkers = totalWorkers;
        options.stopToken = stopToken;
        options.progress = [rowsWrittenAtomic](std::uint64_t rowsWritten, std::uint64_t /*rowsTotal*/) {
            rowsWrittenAtomic->storeRelaxed(static_cast<qint64>(rowsWritten));
        };
        // Match `AnchorManager::Key` canonicalization on Windows.
        options.canonicalizeSourceLocator = [](const std::filesystem::path &path) {
            return logapp::CanonicalLocator(logapp::FsPathToQString(path)).toStdString();
        };
        loglib::WriteSessionBundle(*tablePtr, configSnapshot, destination, options);
    });
    // NOLINTEND(clang-analyzer-webkit.UncountedLambdaCapturesChecker)

    auto *watcher = mSession->EnsureExportWatcher();
    connect(watcher, &LogSession::ExportWatcher::finished, this, &MainWindow::OnExportFinished, Qt::UniqueConnection);
    watcher->setFuture(future);
}

void MainWindow::ShowExportProgress()
{
    if (!mSuppressDialogsForTest)
    {
        if (!mExportProgressDialog)
        {
            mExportProgressDialog = new QProgressDialog(this);
            mExportProgressDialog->setWindowTitle(tr("Exporting"));
            mExportProgressDialog->setWindowModality(Qt::NonModal);
            mExportProgressDialog->setMinimumDuration(EXPORT_DIALOG_DEFER_MS);
            mExportProgressDialog->setRange(0, PROGRESS_PERCENT_MAX);
            mExportProgressDialog->setAutoClose(false);
            mExportProgressDialog->setAutoReset(false);
            connect(mExportProgressDialog.data(), &QProgressDialog::canceled, this, [this]() {
                if (mSession != nullptr && mSession->IsExportInFlight())
                {
                    mSession->MutableExportStopSource().request_stop();
                }
            });
        }
    }

    if (mExportPollTimer == nullptr)
    {
        mExportPollTimer = new QTimer(this);
        mExportPollTimer->setInterval(EXPORT_POLL_INTERVAL_MS);
        connect(mExportPollTimer, &QTimer::timeout, this, &MainWindow::UpdateExportProgressUi);
    }
    mExportPollTimer->start();
    UpdateExportProgressUi();
}

void MainWindow::UpdateExportProgressUi()
{
    bool anyInFlight = false;
    for (LogSession *session : hostedSessions())
    {
        if (session == nullptr || !session->IsExportInFlight())
        {
            continue;
        }
        anyInFlight = true;
        LogSessionView *view = LogSessionViewForSession(session);
        if (view == nullptr)
        {
            continue;
        }
        const qint64 written = session->ExportRowsWritten().loadRelaxed();
        const qint64 total = session->ExportRowsTotal().loadRelaxed();
        int pct = -1;
        QString label;
        if (total > 0)
        {
            pct = static_cast<int>((static_cast<qint64>(PROGRESS_PERCENT_MAX) * written) / total);
            pct = std::min(pct, PROGRESS_PERCENT_MAX);
            label = tr("Exporting %1\n%L2 of %L3 rows")
                        .arg(QFileInfo(session->ExportDestinationPath()).fileName())
                        .arg(written)
                        .arg(total);
        }
        else
        {
            label = tr("Exporting %1\nPreparing\u2026").arg(QFileInfo(session->ExportDestinationPath()).fileName());
        }
        if (view->IsOperationProgressVisible())
        {
            view->UpdateOperationProgress(label, pct);
        }
        else
        {
            view->ShowOperationProgress(label, pct);
        }
    }

    if (!anyInFlight)
    {
        TeardownExportProgress();
        return;
    }

    if (mSuppressDialogsForTest || !mExportProgressDialog)
    {
        return;
    }

    if (mSession == nullptr || !mSession->IsExportInFlight())
    {
        mExportProgressDialog->hide();
        return;
    }

    const qint64 written = mSession->ExportRowsWritten().loadRelaxed();
    const qint64 total = mSession->ExportRowsTotal().loadRelaxed();
    const std::uint64_t generation = mSession->ExportGeneration();
    if (total > 0)
    {
        const int pct = static_cast<int>((static_cast<qint64>(PROGRESS_PERCENT_MAX) * written) / total);
        mExportProgressDialog->setValue(std::min(pct, PROGRESS_PERCENT_MAX));
        if (mSession == nullptr || !mSession->IsExportInFlight() || mSession->ExportGeneration() != generation ||
            !mExportProgressDialog)
        {
            return;
        }
        mExportProgressDialog->setLabelText(tr("Exporting %1 rows to %2\n%L3 of %L4 rows written")
                                                .arg(total)
                                                .arg(QFileInfo(mSession->ExportDestinationPath()).fileName())
                                                .arg(written)
                                                .arg(total));
    }
    else
    {
        mExportProgressDialog->setLabelText(
            tr("Exporting to %1\nPreparing\u2026").arg(QFileInfo(mSession->ExportDestinationPath()).fileName())
        );
    }
}

void MainWindow::TeardownExportProgress()
{
    if (mExportPollTimer != nullptr)
    {
        mExportPollTimer->stop();
    }
    if (mExportProgressDialog)
    {
        mExportProgressDialog->reset();
        mExportProgressDialog->hide();
    }
}

void MainWindow::CancelInFlightExport()
{
    CancelInFlightExportFor(mSession);
}

void MainWindow::CancelInFlightExportFor(LogSession *origin)
{
    if (origin == nullptr)
    {
        return;
    }
    const bool wasInFlight = origin->IsExportInFlight();
    QPointer<LogSessionView> originView = LogSessionViewForSession(origin);
    origin->SetExportInFlight(false);
    auto *watcher = origin->ExportWatcherPtr();
    if (watcher == nullptr)
    {
        origin->ClearExportScratchState();
        if (originView != nullptr)
        {
            originView->HideOperationProgress();
            if (wasInFlight)
            {
                originView->SetContentEnabled(true);
            }
        }
        UpdateExportProgressUi();
        return;
    }
    origin->MutableExportStopSource().request_stop();
    try
    {
        watcher->waitForFinished();
    }
    catch (const std::exception &) // NOLINT(bugprone-empty-catch)
    {
        // The worker is stopping because the session is closing or the
        // window is being destroyed. Cancellation and I/O errors are
        // expected and have no remaining UI to report into.
    }
    watcher->setFuture(QFuture<void>{});
    origin->ClearExportScratchState();
    if (originView != nullptr)
    {
        originView->HideOperationProgress();
        if (wasInFlight)
        {
            originView->SetContentEnabled(true);
        }
    }
    UpdateExportProgressUi();
}

void MainWindow::OnExportFinished()
{
    // Recover export origin from the session-owned watcher's parent.
    // `LogSession::ExportWatcher` is a `QFutureWatcher<void>` type
    // alias, so it has no `Q_OBJECT` macro of its own and cannot
    // be a `qobject_cast` target. Walk the parent chain via the
    // base `QObject*` instead: the watcher was `new
    // ExportWatcher(this)` in `EnsureExportWatcher`, so its
    // parent is exactly the owning `LogSession`.
    const QObject *watcherObj = sender();
    LogSession *origin = (watcherObj != nullptr) ? qobject_cast<LogSession *>(watcherObj->parent()) : nullptr;
    // Direct test invocation without a sender uses the active session.
    if (origin == nullptr)
    {
        if (watcherObj != nullptr)
        {
            return;
        }
        origin = mSession;
    }
    if (mOperations == nullptr)
    {
        return;
    }
    mOperations->CompleteExport(origin);
}

void MainWindow::OnExportFinishedFor(LogSession *origin)
{
    if (origin == nullptr || !origin->IsExportInFlight())
    {
        return;
    }
    const bool hosted = HostedSession(origin->InstanceId()) == origin;
    origin->SetExportInFlight(false);

    if (hosted)
    {
        QPointer<LogSessionView> originViewToReEnable = LogSessionViewForSession(origin);
        if (originViewToReEnable != nullptr)
        {
            originViewToReEnable->HideOperationProgress();
            originViewToReEnable->SetContentEnabled(true);
        }
        UpdateExportProgressUi();
    }

    auto *watcher = origin->ExportWatcherPtr();
    if (watcher == nullptr)
    {
        if (!hosted)
        {
            origin->ClearExportScratchState();
        }
        return;
    }

    QString errorEntry;
    bool cancelled = false;
    try
    {
        // `waitForFinished` re-throws whatever the worker let
        // escape. `QFuture<void>` has no `result()`; wait+observe
        // is the documented idiom.
        watcher->waitForFinished();
    }
    catch (const slv::exports::ExportCancelled &)
    {
        cancelled = true;
    }
    catch (const loglib::SessionBundleCancelled &)
    {
        cancelled = true;
    }
    catch (const std::exception &e)
    {
        const QString label =
            origin->IsExportBundle() ? tr("Failed to export session bundle '%1': %2") : tr("Failed to export '%1': %2");
        errorEntry = label.arg(origin->ExportDestinationPath(), QString::fromLocal8Bit(e.what()));
    }
    catch (...)
    {
        const QString label = origin->IsExportBundle() ? tr("Failed to export session bundle '%1': unknown error")
                                                       : tr("Failed to export '%1': unknown error");
        errorEntry = label.arg(origin->ExportDestinationPath());
    }
    watcher->setFuture(QFuture<void>{});

    const bool wasBundle = origin->IsExportBundle();
    origin->SetExportIsBundle(false);
    if (!hosted)
    {
        origin->ClearExportScratchState();
        return;
    }

    // Shell-level UI (status bar, modal toasts) only fires for the active tab.
    const bool originIsActive = (origin == mSession);

    if (cancelled)
    {
        const QString msg = wasBundle ? tr("Session bundle export cancelled: %1") : tr("Export cancelled: %1");
        const QString text = msg.arg(QFileInfo(origin->ExportDestinationPath()).fileName());
        if (originIsActive)
        {
            statusBar()->showMessage(text, STATUS_BAR_MESSAGE_TIMEOUT_MS);
        }
        else
        {
            origin->QueueStatusMessage(text, STATUS_BAR_MESSAGE_TIMEOUT_MS);
        }
    }
    else if (!errorEntry.isEmpty())
    {
        const QString failureTitle = wasBundle ? tr("Session Bundle Export Failed") : tr("Export Failed");
        ShowParseErrors(failureTitle, {errorEntry.toStdString()}, origin);
        if (originIsActive)
        {
            QMessageBox::warning(this, failureTitle, errorEntry);
        }
        else
        {
            origin->QueueFailureNotice(failureTitle, errorEntry);
            origin->QueueStatusMessage(errorEntry, STATUS_BAR_MESSAGE_TIMEOUT_MS);
        }
    }
    else
    {
        const auto elapsed = std::chrono::steady_clock::now() - origin->ExportStartedAt();
        const qint64 rows = origin->ExportRowsWritten().loadRelaxed();
        const QString msg = wasBundle ? tr("Exported session bundle with %L1 rows to %2 in %3")
                                            .arg(rows)
                                            .arg(QFileInfo(origin->ExportDestinationPath()).fileName())
                                            .arg(HumanDuration(elapsed))
                                      : tr("Exported %L1 rows to %2 (%3) in %4")
                                            .arg(rows)
                                            .arg(QFileInfo(origin->ExportDestinationPath()).fileName())
                                            .arg(origin->ExportFormatLabel())
                                            .arg(HumanDuration(elapsed));
        if (originIsActive)
        {
            statusBar()->showMessage(msg, STATUS_BAR_MESSAGE_TIMEOUT_MS);
        }
        else
        {
            origin->QueueStatusMessage(msg, STATUS_BAR_MESSAGE_TIMEOUT_MS);
        }
        // Only remember on success -- see the deferral note in
        // `ExportFilteredRows`. Safe to call even from background
        // completion (it just updates the "Save to" MRU dir).
        RememberLastExportDir(origin->ExportDestinationPath());
    }

    origin->ClearExportScratchState();

    // Refresh the origin's tab chrome so the export-in-progress
    // glyph clears even for background completions. Safe for the
    // active tab too -- `RefreshTabChrome` is idempotent.
    RefreshTabChrome(origin);
}

void MainWindow::FinalizeAfterDecompressionIfChainTerminal(LogSession *origin)
{
    if (origin == nullptr)
    {
        return;
    }
    LogModel *const model = origin->Model();
    if (model == nullptr)
    {
        return;
    }
    // Another async worker is armed -- let the natural drain point
    // (`OnStreamingFinished` or the next `OnDecompressionFinished`)
    // run instead. Preempting would flip session mode to Idle
    // before `OnStreamingFinished` snapshots it for auto-save.
    if (model->IsStreamingActive() || origin->IsDecompressionInFlight())
    {
        return;
    }

    // Drain both error buckets under their own titles. A chain-
    // terminal decompression failure never re-enters
    // `OnStreamingFinished`, so without this the errors would sit
    // in memory until the next destructive session boundary
    // silently cleared them.
    if (!origin->MutablePendingOpenErrors().empty())
    {
        ShowParseErrors(tr("Error Opening File"), origin->MutablePendingOpenErrors(), origin);
        origin->MutablePendingOpenErrors().clear();
    }
    if (!origin->MutablePendingDecompressionErrors().empty())
    {
        ShowParseErrors(tr("Error Decompressing File"), origin->MutablePendingDecompressionErrors(), origin);
        origin->MutablePendingDecompressionErrors().clear();
    }

    // No session ever armed (all files failed before any parse
    // worker started): `StreamNextPendingFile`'s tail already
    // drained the buckets, so nothing else to do.
    if (!origin->IsSessionActive())
    {
        return;
    }

    // Settle origin state as if `OnStreamingFinished` had fired for the
    // final file. Rows + source stay in place. Snapshot the mode
    // for `AutoSaveSessionSnapshot` before flipping to Idle.
    const SessionMode justFinishedMode = origin->SessionMode();
    origin->SetMode(SessionMode::Idle);
    if (origin == mSession)
    {
        SetConfigurationUiEnabled(true);
        UpdateStreamToolbarVisibility();
        UpdateUi();
        UpdateStreamingStatus();
        UpdateWindowTitle();
    }
    origin->ClearStreamingFileName();
    model->RefreshColumnHealth();
    ApplyDeferredSortFromConfig(origin, LogSessionViewForSession(origin));
    if (ShouldAutoSaveSession(origin, justFinishedMode))
    {
        AutoSaveSessionSnapshot(origin, /*publishOpenWindow=*/true);
    }
}

void MainWindow::OpenLogStream()
{
    const QString file = QFileDialog::getOpenFileName(
        this,
        tr("Open Log Stream..."),
        DefaultOpenDir(),
        tr("Structured Logs (*.json *.jsonl *.ndjson *.logfmt *.csv *.log *.txt);;All Files (*.*)")
    );
    if (file.isEmpty())
    {
        return;
    }
    RememberLastOpenDir(file);
    // User-initiated log-stream open routes through a
    // new foreground tab when the active tab has content. Only
    // fires on the USER path; internal replace-in-place callers
    // (`UndoRotationExpansion`, `OpenLogStreamForTest`) reach
    // `OpenLogStreamFromPath` directly and bypass this so
    // opt-out preferences and rotation-expansion state carry
    // through the transition on the SAME session.
    EnsureFreshActiveTab();
    OpenLogStreamFromPath(file);
}

void MainWindow::OpenLogStreamFromPath(const QString &file)
{
    if (file.isEmpty())
    {
        return;
    }

    // Tab freshening lives on the user-facing wrapper
    // (`OpenLogStream()`), NOT here. `OpenLogStreamFromPath` is
    // also called from `UndoRotationExpansion()` and
    // `OpenLogStreamForTest()`, both of which are internal-
    // replacement paths that must NOT spawn a new tab (they
    // expect the current session's opt-out preferences /
    // rotation-expansion history to carry through the stream-
    // mode transition on the SAME session).

    const size_t retention =
        (mModel->RetentionCap() != 0) ? mModel->RetentionCap() : StreamingControl::RetentionLines();

    // Preserve the effective preference before clearing the source.
    const bool wantRotationHistory = EffectiveAutoDetectRotationHistory();

    // Surface and drop any queued multi-file-open continuation
    // before the AutoSave + destructive reset, so the user sees
    // their discarded selection explicitly rather than having the
    // shared cancel-handler silently clear `mSession->MutablePendingOpenFiles()`.
    // Must run before AutoSave below: `MirrorSessionStateToConfiguration`
    // would otherwise union the never-opened paths into the prior
    // session's persisted locators.
    const int discardedQueuedFiles = static_cast<int>(mSession->MutablePendingOpenFiles().size());
    if (discardedQueuedFiles > 0)
    {
        statusBar()->showMessage(
            tr("Discarded %n queued file(s) before opening log stream.", nullptr, discardedQueuedFiles),
            STATUS_BAR_MESSAGE_TIMEOUT_MS
        );
        mSession->MutablePendingOpenFiles().clear();
    }

    // Flush the outgoing session so user edits made since its last
    // `streamingFinished` survive the destructive reset below.
    // No-op when there's nothing worth saving (live-tail / no uuid).
    // `publishOpenWindow=false` because we `DetachAutoSaveUuid()`
    // immediately afterwards.
    AutoSaveSessionSnapshot(/*publishOpenWindow=*/false);

    // Cancel export and decompression before the reset; see `NewSession`.
    CancelInFlightExport();
    CancelInFlightDecompression();

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
    mSession->SetStreamingErrorsCut(0);
    // Live-tail is transient and not auto-saved; leaving the prior
    // static session's uuid pinned would let closeEvent's
    // `RemoveOpenWindowUuid` drop that session from the multi-
    // window restore set even though the user only switched views.
    DetachAutoSaveUuid();
    // Pending promotion and Undo state belong to the outgoing session.
    ClearPendingLiveTailPromotion();
    ClearRotationExpansionUndoState();

    // Use the static expander to derotate the selected path and build
    // its historical prefix without consulting the outgoing source.
    QStringList siblingPrefix;
    QString tailPath = logapp::CanonicalDisplayPath(file);
    int addedSiblings = 0;
    if (wantRotationHistory)
    {
        const QStringList expanded =
            ExpandLogPathsWithRotationSiblings({file}, addedSiblings, RotationSourceGating::Ignore);
        if (expanded.size() > 1)
        {
            // Oldest-first, series primary last -- live-tail the
            // true primary; everything else is the static prefix.
            siblingPrefix = expanded.mid(0, expanded.size() - 1);
            tailPath = expanded.back();
        }
    }

    // Load history statically before constructing and attaching the tail.
    if (!siblingPrefix.isEmpty())
    {
        mSession->SetPendingOpenFiles(std::move(siblingPrefix));
        mSession->ClearPendingOpenErrors();
        mSession->ClearPendingDecompressionErrors();
        mSession->SetPendingLiveTailPromotion(tailPath, retention);
        // Undo restores the selected path through the live-tail entry point.
        mSession->SetLastRotationExpansion(QStringList{file}, /*wasLiveTail=*/true);
        // Prefix loading must start as a fresh session so the first
        // sibling seeds its source and the all-failed rescue can run.
        mSession->SetMode(SessionMode::Idle);
        mSession->MutableCurrentSource().reset();
        SyncRotationHistoryActionCheckedState();
        StreamNextPendingFile(mSession);
        if (addedSiblings > 0)
        {
            ShowRotationHistoryToast(addedSiblings, file);
        }
        else
        {
            // Derotation changes the open even when it adds no companion.
            if (mActionUndoRotationExpansion != nullptr)
            {
                mActionUndoRotationExpansion->setEnabled(true);
            }
            statusBar()->showMessage(
                tr("Live-tailing %1; loaded older segment(s) as history. Undo via Settings \u25b8 "
                   "Undo rotated history expansion.")
                    .arg(QFileInfo(tailPath).fileName()),
                STATUS_BAR_MESSAGE_TIMEOUT_MS
            );
        }
        return;
    }

    // With no prefix, construct now so open errors remain synchronous.
    // Preserve non-ASCII Windows paths by avoiding code-page narrowing.
    const std::filesystem::path filePath = logapp::QStringToFsPath(tailPath);
    std::unique_ptr<loglib::TailingBytesProducer> source;
    try
    {
        source = std::make_unique<loglib::TailingBytesProducer>(filePath, retention);
    }
    catch (const std::exception &e)
    {
        ShowParseErrors(
            tr("Error Opening Log Stream"),
            {std::string("Failed to open '") + tailPath.toStdString() + "' for streaming: " + e.what()}
        );
        return;
    }

    mSession->SetStreamingFileName(QFileInfo(tailPath).fileName());
    auto &currentSource = mSession->MutableCurrentSource();
    // Live-tail single-file open: populate both arrays so the
    // parallel-array invariant holds across the next save.
    {
        const std::string displayPath = logapp::CanonicalDisplayPath(tailPath).toStdString();
        const std::string dedupKey = logapp::CanonicalLocator(tailPath).toStdString();
        DetectedFormat detected = DetectFormatForPath(filePath);
        currentSource = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File,
            .format = detected.format,
            .locators = {displayPath},
            .locatorDedupKeys = {dedupKey},
            .regexPattern = std::move(detected.regexPattern),
        };
        // `ShouldAutoDetectRotationHistory` already folds the CLI
        // override; no double-AND with `mDisableRotationHistoryOverride`.
        currentSource->followRotationSiblings = ShouldAutoDetectRotationHistory();
    }
    SyncRotationHistoryActionCheckedState();
    mSession->SetMode(SessionMode::LiveTail);
    mSession->ResetStreamingProgress();
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
        currentSource ? currentSource->format : loglib::LogConfiguration::Source::Format::Json;
    std::string regexPattern = currentSource ? currentSource->regexPattern : std::string{};
    auto parserFactory = [format, regexPattern = std::move(regexPattern)]() {
        return MakeParserForFormat(format, regexPattern);
    };
    mModel->BeginStreaming(std::move(streamSource), std::move(options), std::move(parserFactory));
}

void MainWindow::ContinueLiveTailAfterPrefix()
{
    ContinueLiveTailAfterPrefix(mSession);
}

void MainWindow::ContinueLiveTailAfterPrefix(LogSession *origin)
{
    if (origin == nullptr || origin->PendingLiveTailPrimary().isEmpty())
    {
        if (origin != nullptr)
        {
            origin->ClearPendingLiveTailPromotion();
        }
        else
        {
            ClearPendingLiveTailPromotion();
        }
        return;
    }
    LogModel *const model = origin->Model();
    if (model == nullptr)
    {
        return;
    }
    const bool originIsActive = origin == mSession;

    // The all-prefix-files-failed rescue has not disabled this UI yet.
    if (originIsActive)
    {
        SetConfigurationUiEnabled(false);
    }
    const auto [primary, retention] = origin->TakePendingLiveTailPromotion();

    // Construct only after the prefix drains, before callbacks can be missed.
    // Avoid code-page narrowing of non-ASCII Windows paths.
    const std::filesystem::path filePath = logapp::QStringToFsPath(primary);
    std::unique_ptr<loglib::TailingBytesProducer> producer;
    try
    {
        producer = std::make_unique<loglib::TailingBytesProducer>(filePath, retention);
    }
    catch (const std::exception &e)
    {
        ShowParseErrors(
            tr("Error Opening Log Stream"),
            {std::string("Failed to open '") + primary.toStdString() + "' for streaming: " + e.what()},
            origin
        );
        // Keep the loaded prefix, but stop advertising a failed live tail.
        origin->SetMode((model->rowCount() > 0) ? SessionMode::Static : SessionMode::Idle);
        origin->ClearStreamingFileName();
        origin->SetFirstStreamingBatchSeen(false);
        if (originIsActive)
        {
            StopLiveTailTicker();
            SetConfigurationUiEnabled(true);
            UpdateStreamToolbarVisibility();
            UpdateStreamingStatus();
            UpdateWindowTitle();
        }
        return;
    }

    const std::string displayPath = logapp::CanonicalDisplayPath(primary).toStdString();
    const std::string dedupKey = logapp::CanonicalLocator(primary).toStdString();

    auto &currentSource = origin->MutableCurrentSource();
    // Append the primary onto the session's locator list so the
    // full [siblings..., primary] set persists.
    if (currentSource.has_value() && currentSource->kind == loglib::LogConfiguration::Source::Kind::File)
    {
        const bool alreadyPresent = std::any_of(
            currentSource->locatorDedupKeys.begin(),
            currentSource->locatorDedupKeys.end(),
            [&dedupKey](const std::string &existing) { return existing == dedupKey; }
        );
        if (!alreadyPresent)
        {
            loglib::AppendLocator(*currentSource, displayPath, dedupKey);
        }
    }
    else
    {
        // No sibling seeded a source; detect and seed from the primary.
        DetectedFormat detected = DetectFormatForPath(filePath);
        currentSource = loglib::LogConfiguration::Source{
            .kind = loglib::LogConfiguration::Source::Kind::File,
            .format = detected.format,
            .locators = {displayPath},
            .locatorDedupKeys = {dedupKey},
            .regexPattern = std::move(detected.regexPattern),
        };
        // `ShouldAutoDetectRotationHistory` already folds the CLI
        // override; no double-AND with `mDisableRotationHistoryOverride`.
        currentSource->followRotationSiblings = ShouldAutoDetectRotationHistory();
    }
    if (originIsActive)
    {
        SyncRotationHistoryActionCheckedState();
    }

    // `BeginStreaming` resets retention, so apply the captured cap after
    // arming. Warn before that cap trims an oversized static prefix.
    const size_t prefixRows = static_cast<size_t>(std::max(0, model->rowCount()));
    if (retention > 0 && prefixRows > retention)
    {
        const int prefixRowsForPlural = static_cast<int>(std::min<size_t>(prefixRows, std::numeric_limits<int>::max()));
        PostStatusMessage(
            origin,
            tr("Historical prefix (%n line(s)) exceeds the stream retention cap (%L1); older prefix "
               "rows may be trimmed as the tail advances.",
               nullptr,
               prefixRowsForPlural)
                .arg(static_cast<qulonglong>(retention)),
            STATUS_BAR_MESSAGE_TIMEOUT_MS
        );
    }
    origin->SetMode(SessionMode::LiveTail);
    origin->SetStreamingFileName(QFileInfo(primary).fileName());
    if (originIsActive)
    {
        StartLiveTailTicker();
        UpdateStreamingStatus();
        UpdateStreamToolbarVisibility();
        UpdateWindowTitle();
    }
    ApplyDisplayOrder(origin, LogSessionViewForSession(origin));

    auto cfg = std::make_shared<const loglib::LogConfiguration>(model->Configuration());
    loglib::ParserOptions options;
    options.configuration = std::move(cfg);

    auto streamSource = std::make_unique<loglib::StreamLineSource>(filePath, std::move(producer));
    const loglib::LogConfiguration::Source::Format format =
        currentSource ? currentSource->format : loglib::LogConfiguration::Source::Format::Json;
    std::string regexPattern = currentSource ? currentSource->regexPattern : std::string{};
    auto parserFactory = [format, regexPattern = std::move(regexPattern)]() {
        return MakeParserForFormat(format, regexPattern);
    };
    // The rescue path may have no initialized stream to append to.
    if (model->IsStreamingActive() || model->rowCount() > 0)
    {
        model->AppendStreaming(std::move(streamSource), std::move(options), std::move(parserFactory));
    }
    else
    {
        model->BeginStreaming(std::move(streamSource), std::move(options), std::move(parserFactory));
    }
    // Apply the captured cap after either streaming path is armed.
    if (retention > 0)
    {
        model->SetRetentionCap(retention);
    }
}

void MainWindow::OpenLogStreamForTest(const QString &filePath)
{
    OpenLogStreamFromPath(filePath);
}

void MainWindow::OpenStdinStream()
{
    // Refuse interactive stdin before the synchronous peek can
    // block the GUI thread.
    if (loglib::internal::IsStdinInteractive())
    {
        ShowParseErrors(
            tr("Error Opening Standard Input"),
            {tr("Standard input is a terminal, not a pipe or a redirected file. "
                "Pipe log data into the viewer (e.g. `mysvc | StructuredLogViewer -`) or redirect "
                "from a file (`StructuredLogViewer --stdin < mylog.log`).")
                 .toStdString()}
        );
        return;
    }

    // Peek synchronously on the GUI thread before spawning the
    // producer so the peek and the producer's own reads observe
    // the *same* stdin (a single OS-level FD). Any bytes that
    // come after the peek belong to the producer.
    //
    // Cap the wait so slow producers cannot stall startup. Empty
    // or unmatched input defaults to JSON before streaming starts.
    constexpr auto STDIN_PEEK_TIMEOUT = std::chrono::milliseconds(500);
    std::string peek = loglib::internal::StdinPeek(loglib::PROBE_BYTES_BUDGET, STDIN_PEEK_TIMEOUT);

    std::unique_ptr<loglib::StdinBytesProducer> producer;
    try
    {
        producer = std::make_unique<loglib::StdinBytesProducer>();
    }
    catch (const std::exception &e)
    {
        ShowParseErrors(
            tr("Error Opening Standard Input"),
            {tr("Failed to attach to stdin: %1").arg(QString::fromUtf8(e.what())).toStdString()}
        );
        return;
    }

    // Stdin gets its own tab so it cannot clobber file
    // tabs opened alongside it (CLI: `mysvc | slv file.log -`,
    // toolbar: user opens stdin from the popup menu while a file
    // investigation is already open). Only a truly empty active
    // tab is reused. Runs AFTER the peek + producer construction
    // so a producer-create failure does not leave a stray tab.
    EnsureFreshActiveTab();

    OpenStdinStreamFromProducer(std::move(producer), std::move(peek));
}

#ifdef LOGAPP_BUILD_TESTING
void MainWindow::OpenStdinStreamForTest(std::unique_ptr<loglib::BytesProducer> producer, std::string peek)
{
    OpenStdinStreamFromProducer(std::move(producer), std::move(peek));
}
#endif

void MainWindow::OpenStdinStreamFromProducer(std::unique_ptr<loglib::BytesProducer> producer, std::string peek)
{
    if (producer == nullptr)
    {
        return;
    }

    // Tab freshening lives on the user-facing wrapper
    // (`OpenStdinStream()`), NOT here. `OpenStdinStreamForTest`
    // is an internal-replacement path that must NOT spawn a new
    // tab; test fixtures that need multi-tab semantics can call
    // `AddNewTabForTest` themselves.

    // Same rationale as `OpenLogStreamFromPath`: surface and drop
    // the pending queue before AutoSave + reset.
    const int discardedQueuedFiles = static_cast<int>(mSession->MutablePendingOpenFiles().size());
    if (discardedQueuedFiles > 0)
    {
        statusBar()->showMessage(
            tr("Discarded %n queued file(s) before opening standard input.", nullptr, discardedQueuedFiles),
            STATUS_BAR_MESSAGE_TIMEOUT_MS
        );
        mSession->MutablePendingOpenFiles().clear();
    }

    AutoSaveSessionSnapshot(/*publishOpenWindow=*/false);
    CancelInFlightExport();
    CancelInFlightDecompression();

    const SessionSwitchScope switchGuard(*this);

    mModel->Reset();
    ClearAllFilters();
    if (mAnchors != nullptr)
    {
        mAnchors->ClearAll();
    }
    if (mParseErrorsDock != nullptr)
    {
        mParseErrorsDock->ResetSessionState();
    }
    mSession->SetStreamingErrorsCut(0);
    DetachAutoSaveUuid();
    // Pending file-tail and Undo state cannot cross into stdin.
    ClearPendingLiveTailPromotion();
    ClearRotationExpansionUndoState();
    // The outgoing session no longer gates the menu state.
    SyncRotationHistoryActionCheckedState();

    const std::string displayName = producer->DisplayName();
    mSession->SetStreamingFileName(QString::fromStdString(displayName));

    // Detect the format from the peek so the resolved parser is
    // pinned before the first parse batch. Empty peek falls back
    // to JSON (matches `DetectFormatFromBytes`'s "nothing matched"
    // branch), which is a safe default: an empty stdin session
    // produces zero rows either way.
    const loglib::DetectedFormat detected = loglib::DetectFormatFromBytes(peek);
    const auto format = detected.format;
    std::string regexPattern = detected.regexPattern;
    mSession->MutableCurrentSource() = loglib::LogConfiguration::Source{
        .kind = loglib::LogConfiguration::Source::Kind::Stdin,
        .format = format,
        .locators = {displayName},
        .locatorDedupKeys = {displayName},
        .regexPattern = regexPattern,
    };
    mSession->SetMode(SessionMode::LiveTail);
    mSession->ResetStreamingProgress();
    SetConfigurationUiEnabled(false);
    StartLiveTailTicker();
    UpdateStreamingStatus();
    UpdateStreamToolbarVisibility();
    UpdateWindowTitle();
    ApplyDisplayOrder();

    auto config = std::make_shared<const loglib::LogConfiguration>(mModel->Configuration());
    loglib::ParserOptions options;
    options.configuration = std::move(config);
    // Feed the peeked bytes back to the resolved parser so the
    // stream is delivered in full (peek + subsequent bytes).
    options.initialCarry = std::move(peek);

    auto streamSource =
        std::make_unique<loglib::StreamLineSource>(std::filesystem::path(displayName), std::move(producer));
    auto parserFactory = [format, regexPattern = std::move(regexPattern)]() {
        return loglib::MakeParserForFormat(format, regexPattern);
    };
    mModel->BeginStreaming(std::move(streamSource), std::move(options), std::move(parserFactory));

    // Explain the Recent Sessions exclusion.
    statusBar()->showMessage(
        tr("Reading from standard input. This session will not be added to Recent Sessions."),
        STATUS_BAR_MESSAGE_TIMEOUT_MS
    );
}

void MainWindow::OpenNetworkStream()
{
    NetworkStreamDialog dialog(mRegexTemplateRegistry, this);
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

    // Network sessions get their own tab so they cannot
    // clobber other investigations. Only a truly empty active tab
    // is reused. This runs AFTER the dialog + producer construction
    // so a bind / TLS failure surfaces without gratuitously
    // spawning a fresh tab.
    EnsureFreshActiveTab();

    // Same rationale as `OpenLogStreamFromPath`: surface and drop
    // the pending queue before AutoSave + reset.
    const int discardedQueuedFiles = static_cast<int>(mSession->MutablePendingOpenFiles().size());
    if (discardedQueuedFiles > 0)
    {
        statusBar()->showMessage(
            tr("Discarded %n queued file(s) before opening network stream.", nullptr, discardedQueuedFiles),
            STATUS_BAR_MESSAGE_TIMEOUT_MS
        );
        mSession->MutablePendingOpenFiles().clear();
    }

    AutoSaveSessionSnapshot(/*publishOpenWindow=*/false);

    // Cancel export and decompression before the reset; see `NewSession`.
    CancelInFlightExport();
    CancelInFlightDecompression();

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
    mSession->SetStreamingErrorsCut(0);
    DetachAutoSaveUuid();
    // Pending file-tail and Undo state cannot cross into a network stream.
    ClearPendingLiveTailPromotion();
    ClearRotationExpansionUndoState();
    // The outgoing session no longer gates the menu state.
    SyncRotationHistoryActionCheckedState();

    mSession->SetStreamingFileName(QString::fromStdString(displayName));
    // Network-stream locator is a producer URI, not a filesystem
    // path -- no canonicalisation applies, so dedup key == display.
    // Both arrays populated so the parallel-array invariant holds.
    //
    // AutoDetect is session-only. Keep `Json` in the source metadata
    // until `AutoDetectParser` resolves the stream.
    const bool autoDetect = cfg.format == NetworkStreamDialog::Format::AutoDetect;
    const loglib::LogConfiguration::Source::Format dialogFormat = [&] {
        switch (cfg.format)
        {
        case NetworkStreamDialog::Format::Logfmt:
            return loglib::LogConfiguration::Source::Format::Logfmt;
        case NetworkStreamDialog::Format::Csv:
            return loglib::LogConfiguration::Source::Format::Csv;
        case NetworkStreamDialog::Format::Regex:
            return loglib::LogConfiguration::Source::Format::Regex;
        case NetworkStreamDialog::Format::Json:
        case NetworkStreamDialog::Format::AutoDetect:
            break;
        }
        return loglib::LogConfiguration::Source::Format::Json;
    }();
    mSession->MutableCurrentSource() = loglib::LogConfiguration::Source{
        .kind = loglib::LogConfiguration::Source::Kind::NetworkStream,
        .format = dialogFormat,
        .locators = {displayName},
        .locatorDedupKeys = {displayName},
        .regexPattern = cfg.regexPattern.toStdString(),
    };
    mSession->SetMode(SessionMode::LiveTail);
    mSession->ResetStreamingProgress();
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
    // Auto-detect peeks and replays initial bytes; manual formats
    // keep their pinned parser factory.
    if (autoDetect)
    {
        auto parserFactory = []() -> std::unique_ptr<loglib::LogParser> {
            return std::make_unique<loglib::AutoDetectParser>();
        };
        mModel->BeginStreaming(std::move(streamSource), std::move(options), std::move(parserFactory));
        return;
    }
    const auto &currentSource = mSession->CurrentSource();
    const loglib::LogConfiguration::Source::Format format =
        currentSource ? currentSource->format : loglib::LogConfiguration::Source::Format::Json;
    std::string regexPattern = currentSource ? currentSource->regexPattern : std::string{};
    auto parserFactory = [format, regexPattern = std::move(regexPattern)]() {
        return MakeParserForFormat(format, regexPattern);
    };
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
    // on them. `mSession->MutableCurrentSource()` survives -- those rows still came
    // from that source.
    mModel->StopAndKeepRows();
    // A synchronous completion may have promoted the pending live tail.
    if (!IsLiveTailSession())
    {
        mSession->ClearStreamingFileName();
    }
}

void MainWindow::OnRotationDetected()
{
    // Rotation flash lives on `LogSession` so a multi-tab window
    // never projects one tab's flash onto another. The shell's
    // subscription to `rotationFlashChanged` (in the ctor, into
    // `mSessionConnections`) drives `UpdateStreamingStatus` on
    // each edge; the label reads the currently-bound session's
    // `IsRotationFlashActive()` value inside the same slot.
    if (mSession != nullptr)
    {
        mSession->TriggerRotationFlash();
    }
}

void MainWindow::OnSourceStatusChanged(loglib::SourceStatus status)
{
    // Latch `Waiting` so the label keeps showing "Source unavailable".
    mSession->SetSourceWaiting(status == loglib::SourceStatus::Waiting);
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

// Keep file source names compact while preserving stream labels.
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
    // Bundles surface as `Kind::File` (the receiver rewrites the
    // embedded locator to the current path in
    // `OnDecompressionFinished`), so this branch covers them too.
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
    const QString sourceLabel = CurrentSourceLabel(mSession->MutableCurrentSource(), mSession->StreamingFileName());

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
        qsizetype lines = mSession->StreamingLineCount();
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
    // Fold every hosted session's dirty marker so a
    // multi-tab window's `[*]` reflects any dirty tab, not just the
    // active one. Single-session windows resolve to the active
    // session's `IsFiltersDirty()` value.
    AggregateWindowModified();

    // Proxy-icon hint for OS title bars (macOS shows the file glyph;
    // recent Windows uses it for jumplist grouping). Only meaningful
    // for file sources; cleared otherwise (`NetworkStream` locators
    // are producer URIs, not paths). Bundles reach here as
    // `Kind::File` with the locator rebased to the local `.slvbundle`,
    // so the glyph resolves for them too.
    if (const auto &currentSource = mSession->CurrentSource();
        currentSource.has_value() && currentSource->kind == loglib::LogConfiguration::Source::Kind::File &&
        !currentSource->locators.empty())
    {
        setWindowFilePath(QString::fromStdString(currentSource->locators.front()));
    }
    else
    {
        setWindowFilePath(QString());
    }
}

void MainWindow::MarkFiltersDirty()
{
    // Forward to the session; the shell reacts via
    // `filtersDirtyChanged` → `UpdateWindowTitle` connected in the
    // ctor. Keeping the wrapper preserves the call surface every
    // existing MainWindow method already uses.
    if (mSession != nullptr)
    {
        mSession->MarkFiltersDirty();
    }
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

QString MainWindow::DefaultExportDir() const
{
    const QSettings settings;
    QString remembered = settings.value(QStringLiteral("ui/lastExportDir")).toString();
    if (!remembered.isEmpty() && QFileInfo(remembered).isDir())
    {
        return remembered;
    }
    // First-run fallback: seed from the shared "last dialog dir"
    // (one-way -- only exports update `ui/lastExportDir`).
    return DefaultOpenDir();
}

void MainWindow::RememberLastExportDir(const QString &path)
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
    settings.setValue(QStringLiteral("ui/lastExportDir"), dir);
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
// Omit the hour field for sub-hour elapsed times.
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

void MainWindow::PostStatusMessage(const QString &message, int timeoutMs)
{
    PostStatusMessage(mSession, message, timeoutMs);
}

void MainWindow::PostStatusMessage(LogSession *origin, const QString &message, int timeoutMs)
{
    // Background origins queue the toast until that tab is selected.
    if (origin != nullptr && origin != mSession)
    {
        origin->QueueStatusMessage(message, timeoutMs);
        return;
    }
    statusBar()->showMessage(message, timeoutMs);
}

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
    const QString lineCount = loc.toString(static_cast<qlonglong>(mSession->StreamingLineCount()));
    const QString errorCount = loc.toString(static_cast<qlonglong>(mSession->StreamingErrorCount()));

    QString text;
    if (!IsLiveTailSession())
    {
        text = tr("Parsing %1 - %2 lines, %3 errors").arg(mSession->StreamingFileName(), lineCount, errorCount);
    }
    else if (mSession->IsSourceWaiting())
    {
        // Source unavailable takes precedence over Paused.
        text = tr("Source unavailable - last seen %1 - %2 lines, %3 errors")
                   .arg(mSession->StreamingFileName(), lineCount, errorCount);
    }
    else if (mModel->Sink() && mModel->Sink()->IsPaused())
    {
        const auto buffered = static_cast<qlonglong>(mModel->Sink()->PausedLineCount());
        text = tr("Paused - %1 lines, %2 buffered").arg(lineCount, loc.toString(buffered));
    }
    else
    {
        text = tr("Streaming %1 - %2 lines, %3 errors").arg(mSession->StreamingFileName(), lineCount, errorCount);
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

    if (IsLiveTailSession() && mSession->LiveTailElapsedTimer().isValid())
    {
        text += tr(" - %1 since start").arg(FormatElapsed(mSession->LiveTailElapsedTimer().elapsed()));
    }

    if (IsLiveTailSession() && mSession != nullptr && mSession->IsRotationFlashActive())
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
    // every row leaves the counts equal but the expression is still
    // non-match-all, and the user still wants the affordance to
    // clear it. Gate on the configuration expression rather than
    // `mSimpleLeaves` so Advanced-mode subtrees (Or / Not roots, or
    // mixed trees whose non-Leaf remainder lives on
    // `LogConfiguration::expression` after `ApplyAdvancedFilterResult`)
    // keep the button visible even when `mSimpleLeaves` is empty.
    // Mirrors the enabled-state check on `actionClearAllFilters` --
    // the button triggers that action, so the two must agree or a
    // visible button becomes a no-op on click.
    const bool isMatchAll = (mModel != nullptr) && loglib::IsMatchAll(mModel->Configuration().expression);
    mClearFiltersStatusButton->setVisible(!isMatchAll);
}

void MainWindow::StartLiveTailTicker()
{
    mSession->StartLiveTailElapsedTimer();
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
    // Leave the session's live-tail elapsed timer armed so the final
    // status line can still report the session length. It's restarted
    // on the next live-tail open via `StartLiveTailElapsedTimer()`.
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
    // `mThemedActions` paired with the widget that drives its
    // render policy (palette / iconSize / DPR). `RefreshThemedIcons`
    // walks the registry rather than `QToolBar::actions()`, so
    // actions reached through `addWidget` (the split button's
    // default action, its dropdown menu entries) participate in
    // the refresh -- without this they would be wrapped in an
    // internal `QWidgetAction` invisible to a toolbar-iteration
    // refresh and the split button would render blank.
    //
    // Actions without an `svgIconPath` property are skipped by the
    // refresh loop, so actions that ship their own QIcon
    // don't accidentally get clobbered.
    //
    // `svgIconPathChecked` is a second optional property for
    // checkable actions whose On state needs a different glyph
    // (e.g. Pause -> Play when paused). When absent the refresh
    // loop reuses the Off pixmap for the On state, so most actions
    // need only the single tag.
    //
    // `mThemedActions.clear()` defends against any repeated caller
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
    tag(mActionToggleHistogram, mMainToolbar, QStringLiteral(":/icons/bar-chart-3.svg"));
    tag(mActionToggleOverviewRail, mMainToolbar, QStringLiteral(":/icons/bar-chart-horizontal.svg"));
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
    // when `mSimpleLeaves` is empty, which on most styles disables
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
    if (mActionToggleHistogram != nullptr)
    {
        mMainToolbar->addAction(mActionToggleHistogram);
    }
    if (mActionToggleOverviewRail != nullptr)
    {
        mMainToolbar->addAction(mActionToggleOverviewRail);
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
            // Property cleared, e.g. by another caller swapping
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

    if (Filters().empty())
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
    // `mSimpleLeaves` is an unordered_map keyed by UUID, so without
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
    entries.reserve(Filters().size());
    for (const auto &[id, filter] : Filters())
    {
        const int row = ResolveLeafColumnByKeys(filter.columnKeys, mModel->Configuration().columns);
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
    // test seam and programmatic callers.
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
    // Delegate view-local row navigation to `LogSessionView`.
    // Shell forwarder kept so the "Jump to newest" pill click and
    // the auto-follow heuristic (`OnStreamingLineCountChanged`)
    // continue to route through the same entry point.
    if (mSessionView == nullptr)
    {
        return;
    }
    mSessionView->JumpToNewestRow();
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
        mSession->SetMode(SessionMode::Idle);
        break;
    case TestSessionMode::Static:
        mSession->SetMode(SessionMode::Static);
        break;
    case TestSessionMode::LiveTail:
        mSession->SetMode(SessionMode::LiveTail);
        break;
    }
}

MainWindow::TestSessionMode MainWindow::SessionModeForTest() const noexcept
{
    switch (mSession->SessionMode())
    {
    case SessionMode::Idle:
        return TestSessionMode::Idle;
    case SessionMode::Static:
        return TestSessionMode::Static;
    case SessionMode::LiveTail:
        return TestSessionMode::LiveTail;
    }
    Q_UNREACHABLE_RETURN(TestSessionMode::Idle);
}

bool MainWindow::TryLoadAsConfigurationForTest(const QString &file)
{
    return TryLoadAsConfiguration(file);
}

void MainWindow::SetConfigurationUiEnabledForTest(bool enabled)
{
    SetConfigurationUiEnabled(enabled);
}

void MainWindow::TriggerRescueLiveTailForTest(const QString &primary, size_t retention)
{
    // Recreate the all-prefix-files-failed state before promotion.
    mSession->SetPendingLiveTailPromotion(primary, retention);
    mSession->SetMode(SessionMode::Idle);
    mSession->MutableCurrentSource().reset();
    SetConfigurationUiEnabled(true);
    ContinueLiveTailAfterPrefix();
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
    mSession->MutableCurrentSource() = std::move(source);
    // Test fixtures often skip the parallel `locatorDedupKeys`
    // array; backfill so downstream dedup loops behave correctly.
    logapp::BackfillLocatorDedupKeys(mSession->MutableCurrentSource());
}

const std::optional<loglib::LogConfiguration::Source> &MainWindow::CurrentSourceForTest() const noexcept
{
    return mSession->CurrentSource();
}

void MainWindow::OpenFilesForTest(const QStringList &files, OpenMode mode)
{
    // clang-analyzer flags MSVC's <filesystem> bitmask flag-cast on
    // every trace reaching STL filesystem code from this test-only
    // entry point. Suppressing at the innermost sites doesn't cover
    // the diagnostic's trace, so suppress at the entry.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
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
    ApplyDisplayOrder(mSession, mSessionView.data());
}

void MainWindow::ApplyDisplayOrder(LogSession *session, LogSessionView *view)
{
    if (session == nullptr)
    {
        return;
    }
    RowOrderProxyModel *rowOrder = session->RowOrderProxy();
    LogModel *model = session->Model();
    LogTableView *table = view != nullptr ? view->TableView() : nullptr;
    if (rowOrder == nullptr || table == nullptr || model == nullptr)
    {
        return;
    }

    // Static -> static-mode preference; everything else -> stream-mode.
    const bool newestFirst = (session->SessionMode() == SessionMode::Static) ? StreamingControl::IsStaticNewestFirst()
                                                                             : StreamingControl::IsNewestFirst();

    rowOrder->SetReversed(newestFirst);

    table->SetTailEdge(newestFirst ? LogTableView::TailEdge::Top : LogTableView::TailEdge::Bottom);

    // Alternation is permanently off here -- per-level theme
    // colours already partition rows, and toggling it per
    // direction used to flicker on newest-first batches.

    if (model->IsStreamingActive() && session == mSession)
    {
        ScrollToNewestRowIfFollowing();
    }
}

void MainWindow::ShowParseErrors(
    const QString &title, const std::vector<std::string> &errors, LogSession *originatingSession
)
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
    // Route through the originating session (defaults to `mSession`
    // for active-session callers). The dock's `AppendErrorsForSession`
    // decides whether the batch lands in the visible list (active
    // session) or only into the originating session's log
    // (background session).
    LogSession *originating = originatingSession != nullptr ? originatingSession : mSession;
    mParseErrorsDock->AppendErrorsForSession(originating, title, errors);
    if (originating != nullptr && originating != mParseErrorsDock->BoundSession())
    {
        originating->QueueParseErrorsRaise();
    }
}

void MainWindow::ShowDroppedFiltersDialog(int droppedCount, const QString &message, LogSession *origin)
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
    if (origin != nullptr && origin != mSession)
    {
        origin->QueueFailureNotice(QStringLiteral("Filters Dropped on Load"), message);
        return;
    }
    QMessageBox::warning(this, QStringLiteral("Filters Dropped on Load"), message);
}

void MainWindow::MirrorSessionStateToConfiguration()
{
    MirrorSessionStateToConfiguration(mSession);
}

void MainWindow::MirrorSessionStateToConfiguration(LogSession *session)
{
    if (session == nullptr)
    {
        return;
    }
    LogModel *const model = session->Model();
    if (model == nullptr)
    {
        return;
    }
    // Recompose `LogConfiguration::expression` from the live simple-mode
    // leaves merged with any Advanced-mode structure already carried
    // on the expression. The reconciler is session-local and lives on
    // `LogSession`; see
    // `LogSession::RebuildFilterExpressionFromSimpleLeaves` for the
    // preserved invariants (order, Advanced-tree preservation,
    // bare-leaf dedupe).
    session->RebuildFilterExpressionFromSimpleLeaves();

    // Sort mirror is session-local; see
    // `LogSession::MirrorSortToConfiguration` for the deferred-sort
    // exception that preserves the configuration's sort while the
    // proxy is still transitioning through `-1`.
    session->MirrorSortToConfiguration();

    // Drop empty-locator Sources before mirroring: on-disk schema
    // omits `source` when nothing is bound, so a `Source{...,
    // locators: {}}` would round-trip as a label-less recents entry.
    //
    // Multi-file truncation fix: when `session->MutablePendingOpenFiles()` is
    // non-empty, include both already-streamed and still-queued
    // locators so a quit mid-stream persists the full fan-out
    // (the next launch resumes the complete set rather than a
    // strict subset). Dedup via canonical keys.
    const auto &currentSource = session->CurrentSource();
    if (currentSource.has_value() && currentSource->kind == loglib::LogConfiguration::Source::Kind::File &&
        !session->MutablePendingOpenFiles().isEmpty())
    {
        loglib::LogConfiguration::Source mirrored = *currentSource;
        // Seed `seen` with existing dedup keys (case-insensitive on
        // Windows) so pending duplicates of already-streamed paths
        // are skipped.
        std::unordered_set<std::string> seen;
        seen.reserve(mirrored.locatorDedupKeys.size() + static_cast<size_t>(session->MutablePendingOpenFiles().size()));
        for (const std::string &key : mirrored.locatorDedupKeys)
        {
            seen.insert(key);
        }
        for (const QString &pending : session->MutablePendingOpenFiles())
        {
            const std::string displayPath = logapp::CanonicalDisplayPath(pending).toStdString();
            const std::string dedupKey = logapp::CanonicalLocator(pending).toStdString();
            if (seen.insert(dedupKey).second)
            {
                loglib::AppendLocator(mirrored, displayPath, dedupKey);
            }
        }
        model->ConfigurationManager().SetSource(std::move(mirrored));
    }
    else if (loglib::HasLocators(currentSource))
    {
        model->ConfigurationManager().SetSource(currentSource);
    }
    else
    {
        model->ConfigurationManager().SetSource(std::nullopt);
    }

    // Invariant: either no source, or a source with at least one
    // locator. A `Source{kind: ..., locators: {}}` would round-trip
    // as a label-less recents entry.
    {
        const auto &mirrored = model->ConfigurationManager().Configuration().source;
        Q_ASSERT(!mirrored.has_value() || loglib::HasLocators(mirrored));
    }

    // Anchor mirror is session-local; see
    // `LogSession::MirrorAnchorsToConfiguration`.
    session->MirrorAnchorsToConfiguration();
}

bool MainWindow::ShouldAutoSaveSession(SessionMode justFinishedMode) const
{
    return ShouldAutoSaveSession(mSession, justFinishedMode);
}

bool MainWindow::ShouldAutoSaveSession(const LogSession *session, SessionMode justFinishedMode) const
{
    return session != nullptr && session->ShouldAutoSaveAfterStreaming(justFinishedMode);
}

bool MainWindow::AutoSaveSessionSnapshot(bool publishOpenWindow)
{
    return AutoSaveSessionSnapshot(mSession, publishOpenWindow);
}

bool MainWindow::AutoSaveSessionSnapshot(LogSession *session, bool publishOpenWindow)
{
    if (session == nullptr || !session->CanPersistRestorableSnapshot())
    {
        return true;
    }
    if (mHistoryManager == nullptr)
    {
        return false;
    }
#ifdef LOGAPP_BUILD_TESTING
    if (mFailNextAutoSaveForTest)
    {
        mFailNextAutoSaveForTest = false;
        return false;
    }
#endif

    LogModel *const model = session->Model();
    if (model == nullptr)
    {
        return false;
    }

    // Mirror live filters / sort / source so auto-save and the
    // user-driven `SaveSession` path produce the same JSON.
    MirrorSessionStateToConfiguration(session);

    const loglib::LogConfiguration &configuration = model->ConfigurationManager().Configuration();
    // `WriteSnapshotAndPublish` folds the snapshot + open-windows
    // publish under a single cross-process lock. `publishLanded`
    // tells us whether the publish half actually reached disk (it
    // doesn't on contention or when the `--new-instance` gate is
    // off); use it to drive the latch so retries stay coherent.
    bool publishLanded = false;
    const QString uuid = mHistoryManager->WriteSnapshotAndPublish(
        configuration, session->AutoSaveUuid(), /*publishOpenWindow=*/publishOpenWindow, &publishLanded
    );
    if (uuid.isEmpty())
    {
        // Save failed. The atomic temp+rename in
        // `LogConfigurationManager::Save` preserves any prior valid
        // `<uuid>.json`; the existing pins still point there.
        return false;
    }
    // Pin so subsequent auto-saves rewrite the same JSON instead
    // of cluttering recents.
    session->SetAutoSaveUuid(uuid);
    if (publishLanded)
    {
        session->SetAutoSaveUuidPublished(true);
    }
    return true;
}

void MainWindow::DetachAutoSaveUuid()
{
    mSession->DetachAutoSaveUuid();
}

QString MainWindow::RestorableActiveSessionUuid() const noexcept
{
    return mSession->RestorableSessionUuid();
}

QStringList MainWindow::RestorableHostedSessionUuids() const
{
    QStringList result;
    for (const LogSession *session : hostedSessions())
    {
        if (session == nullptr)
        {
            continue;
        }
        const QString uuid = session->RestorableSessionUuid();
        if (!uuid.isEmpty())
        {
            result.append(uuid);
        }
    }
    return result;
}

void MainWindow::AutoSaveAllHostedSessions(bool publishOpenWindow)
{
    if (mOperations == nullptr)
    {
        AutoSaveSessionSnapshot(publishOpenWindow);
        return;
    }
    mOperations->SaveAllHostedSnapshots(publishOpenWindow);
}

QString MainWindow::WorkspaceWindowUuid() const
{
    // Lazily allocate the identity on first read. `mutable`
    // avoids threading the pointer through a non-const getter.
    if (mWorkspaceWindowUuid.isEmpty())
    {
        mWorkspaceWindowUuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    return mWorkspaceWindowUuid;
}

namespace
{

// Keep runtime-to-persistence source-mode coupling at one boundary.
slv::persistence::SourceMode SourceModeFor(const LogSession *session)
{
    using slv::persistence::SourceMode;
    if (session == nullptr)
    {
        return SourceMode::Empty;
    }
    const auto &src = session->CurrentSource();
    if (!src.has_value())
    {
        // Distinguish "config-only" (loaded configuration snapshot but no
        // bound source, e.g. reopened from a Recent Sessions
        // entry captured via File -> Save Session As before any
        // logs were opened) from "empty" (fresh untitled tab).
        // Config-only sessions publish a stable uuid via
        // `RestorableSessionUuid()` (pinned `mAutoSaveUuid` +
        // no source); empty sessions do not. Before the fix,
        // this branch always returned `Empty`, so
        // `ApplyWorkspaceWindow`'s `isFilePath` gate skipped
        // the tab and the config-only investigation restored
        // as blank on the next launch.
        return session->RestorableSessionUuid().isEmpty() ? SourceMode::Empty : SourceMode::ConfigOnly;
    }
    const auto kind = src->kind;
    if (kind == loglib::LogConfiguration::Source::Kind::Stdin)
    {
        return SourceMode::Stdin;
    }
    if (kind == loglib::LogConfiguration::Source::Kind::NetworkStream)
    {
        return SourceMode::Network;
    }
    // File-kind sub-modes rely on runtime state (live-tail
    // latch, bundle intent, decompression original path).
    if (session->IsLiveTailSession())
    {
        return SourceMode::LiveTailFile;
    }
    if (!session->DecompressionOriginalPath().isEmpty())
    {
        // Session was opened from an archive. Bundle sessions
        // additionally arm `ShouldApplyEmbeddedBundleConfig`.
        return session->ShouldApplyEmbeddedBundleConfig() ? slv::persistence::SourceMode::Bundle
                                                          : SourceMode::Compressed;
    }
    if (src->locators.size() > 1)
    {
        return SourceMode::MultiFile;
    }
    return SourceMode::File;
}

} // namespace

slv::persistence::WorkspaceWindow MainWindow::CaptureWorkspaceWindow() const
{
    slv::persistence::WorkspaceWindow snapshot;
    snapshot.windowUuid = WorkspaceWindowUuid();
    snapshot.geometry = saveGeometry();
    snapshot.dockState = saveState();
    snapshot.activeTabIndex = (mTabWidget != nullptr) ? mTabWidget->currentIndex() : 0;
    for (const LogSession *session : hostedSessions())
    {
        slv::persistence::WorkspaceTab tab;
        if (session != nullptr)
        {
            tab.sessionUuid = session->RestorableSessionUuid();
            tab.sourceMode = SourceModeFor(session);
        }
        snapshot.tabs.push_back(std::move(tab));
    }
    if (snapshot.activeTabIndex < 0 || std::cmp_greater_equal(snapshot.activeTabIndex, snapshot.tabs.size()))
    {
        // Empty tab strip and out-of-range indices both fall
        // back to the first tab; kept as a single assignment
        // so `bugprone-branch-clone` stays quiet.
        snapshot.activeTabIndex = 0;
    }
    return snapshot;
}

void MainWindow::ApplyWorkspaceWindow(const slv::persistence::WorkspaceWindow &window, std::uint64_t generation)
{
    // Adopt the persisted uuid so future publishes overwrite
    // rather than duplicate. Empty stays empty; a first-time
    // capture allocates on demand via `WorkspaceWindowUuid()`.
    if (!window.windowUuid.isEmpty())
    {
        mWorkspaceWindowUuid = window.windowUuid;
    }

    if (mTabWidget == nullptr)
    {
        return;
    }

    // The freshly-constructed window already has one Untitled
    // tab. Reuse it for the first restored tab if any exists
    // so the restore does not leave a leading blank.
    bool firstTabReused = false;
    for (std::size_t i = 0; i < window.tabs.size(); ++i)
    {
        const auto &tab = window.tabs[i];
        const LogSession *targetSession = nullptr;
        if (!firstTabReused)
        {
            targetSession = SessionAtTab(0);
            firstTabReused = true;
        }
        else
        {
            const SessionInstanceId newId = AddNewTab(/*makeActive=*/false);
            (void)newId;
            targetSession = SessionAtTab(static_cast<int>(mTabs.size()) - 1);
        }
        if (targetSession == nullptr)
        {
            continue;
        }
        if (tab.restorePolicy == slv::persistence::RestorePolicy::Skip)
        {
            // Slot reserved; no restore work. The tab stays
            // Untitled + empty.
            continue;
        }
        // Reopen file-backed and live-tail sessions from the
        // generation snapshot, falling back to recents. Network
        // and stdin tabs stay empty. Missing or corrupt snapshots
        // leave this tab empty and do not abort later tabs.
        const bool isFilePath = tab.sourceMode == slv::persistence::SourceMode::File ||
                                tab.sourceMode == slv::persistence::SourceMode::MultiFile ||
                                tab.sourceMode == slv::persistence::SourceMode::Compressed ||
                                tab.sourceMode == slv::persistence::SourceMode::Bundle ||
                                tab.sourceMode == slv::persistence::SourceMode::LiveTailFile ||
                                tab.sourceMode == slv::persistence::SourceMode::ConfigOnly;
        if (!isFilePath || tab.sessionUuid.isEmpty())
        {
            continue;
        }
        QString jsonPath = slv::persistence::WorkspacePersistence::SessionSnapshotPath(generation, tab.sessionUuid);
        if (jsonPath.isEmpty() || !QFileInfo::exists(jsonPath))
        {
            jsonPath = (mHistoryManager != nullptr) ? mHistoryManager->PathForUuid(tab.sessionUuid) : QString{};
        }
        if (jsonPath.isEmpty() || !QFileInfo::exists(jsonPath))
        {
            continue;
        }
        // The activate is safe: the caller's saved active-tab index is
        // applied after this loop so the visual current tab still
        // matches `window.activeTabIndex`.
        mTabWidget->setCurrentIndex(static_cast<int>(i));
        OpenSessionFromJson(tab.sessionUuid, jsonPath, /*informIfNonFile=*/false);
    }

    // Select the saved active tab AFTER every restore has
    // bound so the alias / dock swap only runs once per
    // launch on the tab the user actually wanted focused.
    const int activeIdx = std::clamp(window.activeTabIndex, 0, mTabWidget->count() - 1);
    if (activeIdx >= 0)
    {
        mTabWidget->setCurrentIndex(activeIdx);
    }

    // Apply saved chrome LAST so it lands after every dock
    // has rebound. Empty blobs are silently ignored by Qt.
    if (!window.dockState.isEmpty())
    {
        (void)restoreState(window.dockState);
    }
    if (!window.geometry.isEmpty())
    {
        (void)restoreGeometry(window.geometry);
    }
}

QString MainWindow::ClosePromptInformativeTextForTest(const LogSession &session)
{
    return ClosePromptInformativeText(session);
}

QString MainWindow::ClosePromptInformativeText(const LogSession &session)
{
    const SessionPresentationSnapshot snapshot = session.PresentationSnapshot();
    if (snapshot.dirty.restorableInPlace && !snapshot.dirty.ephemeralUnreproducible)
    {
        return QString();
    }
    return QObject::tr(
        "Save writes session settings such as columns, filters, and highlights. "
        "The source connection and retained rows are not included."
    );
}

bool MainWindow::PrepareSessionClose(LogSession *closing)
{
    if (closing == nullptr)
    {
        return true;
    }
    // Editor edits live in the shared widget until a tab switch
    // captures them. Close, New Session, and destructive open must
    // see that buffer before `CloseDecision()` runs.
    if (!mHighlightRulesEditor.isNull() && mHighlightRulesEditorSession.data() == closing)
    {
        CaptureHighlightRulesEditorDraft();
    }
    bool proceed = false;
    switch (closing->CloseDecision())
    {
    case SessionCloseDecision::Silent:
        proceed = true;
        break;
    case SessionCloseDecision::Autosave:
    {
        if (!AutoSaveSessionSnapshot(closing, /*publishOpenWindow=*/false))
        {
#ifdef LOGAPP_BUILD_TESTING
            if (!mSuppressDialogsForTest)
#endif
            {
                QMessageBox::warning(
                    this,
                    tr("Could Not Save Session"),
                    tr("The session could not be saved, so it was not closed. "
                       "Check that the sessions folder is writable and try again.")
                );
            }
            proceed = false;
            break;
        }
        proceed = true;
        break;
    }
    case SessionCloseDecision::Prompt:
#ifdef LOGAPP_BUILD_TESTING
        if (!mClosePromptChoicesForTest.empty())
        {
            const ClosePromptChoiceForTest choice = mClosePromptChoicesForTest.front();
            mClosePromptChoicesForTest.erase(mClosePromptChoicesForTest.begin());
            proceed = choice == ClosePromptChoiceForTest::Discard;
            break;
        }
        if (mSuppressDialogsForTest)
        {
            proceed = true;
            break;
        }
#endif
        proceed = PromptSaveDiscardCancel(closing);
        break;
    }
    if (proceed)
    {
        // Discarded or saved editor work must not remain on a reused session.
        closing->ClearHighlightEditorDraft();
    }
    return proceed;
}

bool MainWindow::PromptSaveDiscardCancel(LogSession *closing)
{
    const SessionPresentationSnapshot snapshot = closing->PresentationSnapshot();
    QString label = snapshot.sourceLabel;
    if (label.isEmpty())
    {
        label = snapshot.shortLabel;
    }
    if (label.isEmpty())
    {
        label = tr("this session");
    }

    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Close Session"));
    box.setText(tr("%1 has unsaved changes.").arg(label));
    const QString informative = ClosePromptInformativeText(*closing);
    if (!informative.isEmpty())
    {
        box.setInformativeText(informative);
    }
    const QPushButton *const saveBtn = box.addButton(tr("Save"), QMessageBox::AcceptRole);
    const QPushButton *const discardBtn = box.addButton(tr("Discard"), QMessageBox::DestructiveRole);
    QPushButton *const cancelBtn = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancelBtn);
    box.exec();
    if (box.clickedButton() == cancelBtn)
    {
        return false;
    }
    if (box.clickedButton() == discardBtn)
    {
        return true;
    }
    if (box.clickedButton() != saveBtn)
    {
        return false;
    }

    const int previousIndex = (mTabWidget != nullptr) ? mTabWidget->currentIndex() : -1;
    const int closingIndex = TabIndexForSession(closing->InstanceId());
    if (mTabWidget != nullptr && closingIndex >= 0 && mTabWidget->currentIndex() != closingIndex)
    {
        mTabWidget->setCurrentIndex(closingIndex);
    }
    const bool saved = SaveSession();
    if (!saved && mTabWidget != nullptr && previousIndex >= 0 && mTabWidget->currentIndex() != previousIndex)
    {
        mTabWidget->setCurrentIndex(previousIndex);
    }
    return saved;
}

void MainWindow::EnsureFreshActiveTab()
{
    // Reuse a blank landing tab when it has no bound
    // source AND zero rows is the fresh / just-cleared state
    // where reusing the slot is preferable to leaving an empty
    // tab behind. We intentionally do NOT gate on
    // `IsFiltersDirty()` because `NewSession()` marks the fresh
    // session dirty via `ClearAllFilters()`, and gating on it
    // would fork behaviour every time a user cleared the
    // workspace before an open.
    const bool activeTabIsEmpty = (mSession != nullptr) && !mSession->CurrentSource().has_value() &&
                                  (mModel == nullptr || mModel->rowCount(QModelIndex{}) == 0);
    if (!activeTabIsEmpty)
    {
        AddNewTab(/*makeActive=*/true);
    }
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

    // Confirm every hosted session before any irreversible close work.
    // Autosave and prompts identify the session without requiring it
    // to be active. Restore the prior tab when the user cancels.
    const int previousActiveIndexForPrompt = (mTabWidget != nullptr) ? mTabWidget->currentIndex() : -1;
    bool cancelled = false;
    for (int idx = 0; idx < static_cast<int>(mTabs.size()); ++idx)
    {
        LogSession *session = SessionAtTab(idx);
        if (session == nullptr)
        {
            continue;
        }
        if (!PrepareSessionClose(session))
        {
            cancelled = true;
            break;
        }
    }
    if (cancelled)
    {
        if (mTabWidget != nullptr && previousActiveIndexForPrompt >= 0 &&
            previousActiveIndexForPrompt < mTabWidget->count() &&
            mTabWidget->currentIndex() != previousActiveIndexForPrompt)
        {
            mTabWidget->setCurrentIndex(previousActiveIndexForPrompt);
        }
        event->ignore();
        return;
    }

    // Sample every session before cancellation; dirty confirmation has already completed.
    // Session destructors perform the blocking worker drain before their atomics are destroyed.
    for (const LogSession *session : hostedSessions())
    {
        if (session == nullptr)
        {
            continue;
        }
        // Read only; hand-rolled `static_cast<std::uint32_t>` is
        // avoided by the overloaded `operator&(std::uint32_t,
        // SessionClosePreconditions)` declared alongside the enum
        // in `log_session_presentation.hpp:187`.
        const std::uint32_t preconditions = session->PreCheckClose();
        const bool decompressionInFlight = (preconditions & SessionClosePreconditions::DecompressionInFlight) != 0U;
        const bool exportInFlight = (preconditions & SessionClosePreconditions::ExportInFlight) != 0U;
        // The destructive cancellation path below handles both in-flight states.
        (void)decompressionInFlight;
        (void)exportInFlight;
    }

    // Persist geometry/dock layout before tear-down. Best-effort: a
    // QSettings write failure is silently swallowed alongside the
    // auto-save failures below.
    SaveWindowChrome();

    // Cancel + drain any in-flight decompression before touching
    // session state, so its `finished` callout can't fire against
    // a MainWindow whose `mSession->MutableCurrentSource()` / `mSessionMode` we're
    // about to reset. The helper is a bounded blocking wait and
    // also tears down the progress dialog + poll timer.
    //
    // Runs BEFORE `AutoSaveSessionSnapshot`: the in-flight file was
    // never successfully parsed so its locator isn't in
    // `mSession->MutableCurrentSource()`; the auto-save loses nothing. Running the
    // cancel *after* would risk the finished callout re-entering
    // user code (nested event loops from `QMessageBox` inside
    // `ShowParseErrors`) between the snapshot and the state reset.
    CancelInFlightDecompression();
    // Cancel the export worker here (closeEvent runs before
    // ~MainWindow); the destructor's Cancel is a defensive backstop.
    CancelInFlightExport();
    // Sibling tabs' workers also need a stop-request so their
    // futures don't deliver into a half-destructed shell. The
    // synchronous drain still lives in `~LogSession()`
    // (`log_session.cpp:157-177`); this loop only issues the
    // stop-request. Mirrors the pattern in `~MainWindow`.
    for (LogSession *session : hostedSessions())
    {
        if (session == nullptr || session == mSession)
        {
            continue;
        }
        session->MutableDecompressionStopSource().request_stop();
        session->MutableExportStopSource().request_stop();
    }

    // Flush restorable static sessions into Recent Sessions, then
    // detach. Live-tail and non-file sources stay out of recents;
    // quit persistence uses `CanPersistRestorableSnapshot()` instead.
    // Do not publish closing tabs into the open-window index.
    const int tabCountAtClose = mTabWidget != nullptr ? mTabWidget->count() : 0;
    if (tabCountAtClose > 0)
    {
        for (int idx = 0; idx < tabCountAtClose; ++idx)
        {
            if (mTabWidget->currentIndex() != idx)
            {
                mTabWidget->setCurrentIndex(idx);
            }
            if (mSession != nullptr && ShouldAutoSaveSession(mSession, mSession->EffectiveTerminalMode()))
            {
                AutoSaveSessionSnapshot(/*publishOpenWindow=*/false);
            }
            DetachAutoSaveUuid();
            if (mSession != nullptr)
            {
                mSession->MutableCurrentSource().reset();
                mSession->ResetMode();
            }
        }
    }
    else
    {
        // Defensive: no `mTabWidget` (headless / early-teardown).
        if (mSession != nullptr && ShouldAutoSaveSession(mSession, mSession->EffectiveTerminalMode()))
        {
            AutoSaveSessionSnapshot(/*publishOpenWindow=*/false);
        }
        DetachAutoSaveUuid();
        mSession->MutableCurrentSource().reset();
        mSession->ResetMode();
    }
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

bool MainWindow::SaveSession()
{
    // A stdin session's `<stdin>` locator cannot be reopened; a `Full` save would
    // serialise a stanza the load path can only reject with an info
    // popup. Offer to demote to `ColumnsOnly` so columns/filters/
    // highlights round-trip cleanly and the on-disk config omits the
    // useless source field entirely.
    loglib::SaveScope effectiveScope = loglib::SaveScope::Full;
    if (const auto &currentSource = mSession->CurrentSource();
        currentSource.has_value() && currentSource->kind == loglib::LogConfiguration::Source::Kind::Stdin)
    {
#ifdef LOGAPP_BUILD_TESTING
        if (mSuppressDialogsForTest)
        {
            effectiveScope = loglib::SaveScope::ColumnsOnly;
        }
        else
#endif
        {
            const auto choice = QMessageBox::question(
                this,
                tr("Save Session (stdin)"),
                tr("This session was read from standard input, which cannot be reopened later. "
                   "The columns, filters, and highlights can still be saved so they apply to a "
                   "future run.\n\nSave columns and filters only?"),
                QMessageBox::Save | QMessageBox::Cancel,
                QMessageBox::Save
            );
            if (choice != QMessageBox::Save)
            {
                return false;
            }
            effectiveScope = loglib::SaveScope::ColumnsOnly;
        }
    }

    const QString file =
        QFileDialog::getSaveFileName(this, tr("Save Session"), DefaultOpenDir(), tr("JSON (*.json);;All Files (*)"));
    if (file.isEmpty())
    {
        return false;
    }
    RememberLastOpenDir(file);
    try
    {
        DoSaveConfiguration(file, effectiveScope);
        return true;
    }
    catch (std::exception &e)
    {
        QMessageBox::warning(this, "Error Saving Session", e.what());
        return false;
    }
}

void MainWindow::DoSaveConfiguration(const QString &path, loglib::SaveScope scope)
{
    // Mirror unconditionally even though every mutation point already
    // keeps the configuration current -- documents intent and
    // protects against a mutator that forgets to mirror.
    // `scope` selects which subset lands on disk; `Save` throws on
    // I/O failure (callers catch).
    MirrorSessionStateToConfiguration();
    mModel->ConfigurationManager().Save(path.toStdString(), scope);
    // Save succeeded — runtime now matches disk, so drop `[*]`.
    // A throw above (correctly) skips this and leaves the marker set.
    // The session emits `filtersDirtyChanged(false)` which drives
    // the title refresh through the ctor-installed connection.
    mSession->ClearFiltersDirty();
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
            mDiagnosticsDialog, &ConfigurationDiagnosticsDialog::editColumnRequested, this, [this](int columnIndex) {
                if (mDiagnosticsDialogSession.isNull() || mDiagnosticsDialogSession.data() != mSession)
                {
                    return;
                }
                if (HostedSession(mDiagnosticsDialogSession->InstanceId()) != mDiagnosticsDialogSession.data())
                {
                    return;
                }
                EditColumn(columnIndex);
            }
        );
    }
    // Capture the originating session so `RebindSharedDocks`
    // can close the dialog on tab switch (dialog holds a
    // QPointer<LogModel> captured at construction; letting it
    // survive across a session swap would silently point every
    // subsequent Refresh at a stale model).
    mDiagnosticsDialogSession = mSession;
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
    // Capture the originating session so `RebindSharedDocks`
    // can close the dialog on tab switch. Same rationale as the
    // diagnostics dialog above -- the dialog's captured
    // QPointer<LogModel> must not silently span a session swap.
    mColumnsManagerDialogSession = mSession;
    mColumnsManagerDialog->SetOriginSession(mSession);
    mColumnsManagerDialog->Refresh();
    mColumnsManagerDialog->show();
    mColumnsManagerDialog->raise();
    mColumnsManagerDialog->activateWindow();
}

namespace
{
// Map through both proxies and reject invalid links.
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
    // Scope this into `mSessionConnections` so a tab
    // switch (or destructor teardown) reaps this against the old
    // selection model before the new view's selection model takes
    // over.
    mSessionConnections += connect(
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
        // Reached both programmatically and via
        // `FindRecordWidget::RequestMatchCountSoon` after the user
        // cleared the field. `InvalidateFindMatchCache` drops the
        // cache and pushes an empty list into the rail so its ticks
        // clear in one round-trip.
        InvalidateFindMatchCache();
        mFindRecord->SetMatchInfo(0, 0);
        return;
    }

    // Rebuild only when the needle / flags actually changed. A
    // Next / Previous click reports the same needle, so the second
    // call just resolves the new `i` via binary search below.
    auto &findCacheOpt = mSession->MutableFindMatchCacheState();
    const bool cacheHit = findCacheOpt.has_value() && findCacheOpt->needle == text &&
                          findCacheOpt->wildcards == wildcards &&
                          findCacheOpt->regularExpressions == regularExpressions;
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
        const int proxyRowCount = mSortFilterProxyModel->rowCount();
        // Read the rail's live bucket count so per-hit counters can
        // fold straight into a fixed-size vector during the scan.
        // Zero when the rail is hidden — the bucket fold is skipped
        // in that case.
        const std::size_t nBuckets =
            (mOverviewRailModel != nullptr) ? mOverviewRailModel->BucketCount() : std::size_t{0};

        // Single-walk accumulator. `sortedRows` is capped at
        // `MAX_FIND_MATCH_COUNT` for the Next / Previous binary
        // search; the rail is fed via `bucketCounts`. Once every
        // bucket has a tick and the navigator list is past the cap
        // we can stop: further hits only change density, and paint
        // is presence-only. Sparse needles still scan to the end.
        std::vector<int> sortedRows;
        // Clamp against `rowCount() == -1` (some models return -1
        // for "unknown"); a negative cast to size_t would trigger
        // a multi-EB `reserve` and throw on the GUI thread.
        const int reserveHint = std::min(std::max(0, proxyRowCount), MAX_FIND_MATCH_COUNT);
        sortedRows.reserve(static_cast<size_t>(reserveHint));
        std::vector<uint32_t> bucketCounts(nBuckets, uint32_t{0});
        uint32_t totalMatches = 0;
        std::size_t bucketsHit = 0;
        bool scanExhausted = true;
        // `ForEachMatchingRow` streams matches without allocating a
        // `QList<QModelIndex>`. Ascending row order + no duplicates
        // is contracted, so `sortedRows` stays sorted-unique for
        // the binary search.
        mSortFilterProxyModel->ForEachMatchingRow(
            start,
            Qt::DisplayRole,
            value,
            flags,
            /*forward=*/true,
            /*skipFirstN=*/0,
            [&](const QModelIndex &matchIndex) -> bool {
                ++totalMatches;
                const int proxyRow = matchIndex.row();
                if (nBuckets > 0 && proxyRowCount > 0 && proxyRow >= 0)
                {
                    const std::size_t bucketIdx =
                        (static_cast<std::size_t>(proxyRow) * nBuckets) / static_cast<std::size_t>(proxyRowCount);
                    uint32_t &slot = bucketCounts[std::min(bucketIdx, nBuckets - 1)];
                    if (slot == 0)
                    {
                        ++bucketsHit;
                    }
                    ++slot;
                }
                if (static_cast<int>(sortedRows.size()) < MAX_FIND_MATCH_COUNT)
                {
                    sortedRows.push_back(proxyRow);
                }
                // Early-exit once overflow is proven and the rail
                // is either hidden or has a tick in every bucket.
                // Further hits only change density, which paint
                // ignores; the label reads "+" past the cap.
                if (totalMatches > static_cast<uint32_t>(MAX_FIND_MATCH_COUNT) &&
                    (nBuckets == 0 || bucketsHit >= nBuckets))
                {
                    scanExhausted = false;
                    return false;
                }
                return true;
            }
        );

        const bool overflowed = !scanExhausted || totalMatches > static_cast<uint32_t>(MAX_FIND_MATCH_COUNT);
        // Defensive sort/dedup: contract violations would silently
        // corrupt the binary search. Assert in debug, warn in
        // release, still recover so the caller doesn't see garbage.
        const bool sortedAsExpected = std::ranges::is_sorted(sortedRows);
        Q_ASSERT(sortedAsExpected);
        if (!sortedAsExpected)
        {
            qWarning() << "MainWindow::UpdateFindMatchCount: ForEachMatchingRow returned unsorted rows; "
                          "sorting defensively. This is a contract violation worth investigating.";
            std::ranges::sort(sortedRows);
        }
        const bool dedupedAsExpected = std::ranges::adjacent_find(sortedRows) == sortedRows.end();
        Q_ASSERT(dedupedAsExpected);
        if (!dedupedAsExpected)
        {
            qWarning() << "MainWindow::UpdateFindMatchCount: ForEachMatchingRow returned duplicate rows; "
                          "deduplicating defensively. This is a contract violation worth investigating.";
            sortedRows.erase(std::ranges::unique(sortedRows).begin(), sortedRows.end());
        }
        findCacheOpt = FindMatchCache{
            .needle = text,
            .wildcards = wildcards,
            .regularExpressions = regularExpressions,
            .overflowed = overflowed,
            .sortedRows = std::move(sortedRows),
            .totalMatches = totalMatches,
            // Keep a copy so a find-dock reveal / rail re-show can
            // push unbiased ticks without re-walking the proxy.
            // Empty when the rail had zero buckets (hidden).
            .bucketCounts = (nBuckets > 0) ? bucketCounts : std::vector<uint32_t>{},
        };
        // Prefer the bucket-counts path when the rail is armed: it
        // avoids the O(matches) allocation and doesn't bias toward
        // the top of the log when `sortedRows` is capped. Fall back
        // to the row-list path when hidden so a later re-show can
        // still restore ticks.
        if (mOverviewRailModel != nullptr)
        {
            if (nBuckets > 0)
            {
                mOverviewRailModel->SetMatchBucketCounts(std::move(bucketCounts), totalMatches);
            }
            else
            {
                mOverviewRailModel->SetMatchProxyRows(findCacheOpt->sortedRows);
            }
        }
    }

    // Under `overflowed` `totalMatches` is a lower bound and the
    // position lookup below can return `0` for a cursor past the
    // cap; the "+" suffix on the label signals both.
    const int total = static_cast<int>(findCacheOpt->totalMatches);
    int currentOneBased = 0;
    if (total > 0 && mTableView != nullptr && !findCacheOpt->sortedRows.empty())
    {
        const QModelIndex currentIdx = mTableView->currentIndex();
        if (currentIdx.isValid())
        {
            const auto begin = findCacheOpt->sortedRows.begin();
            const auto end = findCacheOpt->sortedRows.end();
            const auto it = std::lower_bound(begin, end, currentIdx.row());
            if (it != end && *it == currentIdx.row())
            {
                currentOneBased = static_cast<int>(it - begin) + 1;
            }
        }
    }
    mFindRecord->SetMatchInfo(currentOneBased, total, findCacheOpt->overflowed);
}

void MainWindow::InvalidateFindMatchCache()
{
    mSession->ResetFindMatchCache();
    // Drop rail ticks alongside the cache so a stale find selection
    // can't strand ticks on rows the current filter rejects.
    if (mOverviewRailModel != nullptr)
    {
        mOverviewRailModel->SetMatchProxyRows({});
    }
}

void MainWindow::PushFindMatchesToOverviewRail()
{
    auto &findCacheOpt = mSession->MutableFindMatchCacheState();
    if (mOverviewRailModel == nullptr || !findCacheOpt.has_value())
    {
        return;
    }
    // Match ticks mirror the find indicator: only while the bar
    // is visible. The cache still survives a close / tab-hide so
    // `FindDock::revealed` can restore without re-scanning; do
    // not re-apply ticks from a stale cache while the bar is
    // hidden (e.g. rail toggled off→on after find was closed).
    if (!IsFindBarVisible())
    {
        return;
    }
    const std::size_t nBuckets = mOverviewRailModel->BucketCount();
    if (nBuckets == 0)
    {
        // Rail hidden: durable model state (row list or stored
        // bucket counts) is enough for the next SetBucketCount(H).
        return;
    }

    const FindMatchCache &cache = *findCacheOpt;
    if (cache.bucketCounts.size() == nBuckets)
    {
        mOverviewRailModel->SetMatchBucketCounts(cache.bucketCounts, cache.totalMatches);
        return;
    }

    // Bucket counts missing or size-mismatched (rail was hidden
    // during the scan, or H changed since). Prefer a fresh recount
    // so the rail never paints a top-biased capped strip.
    if (!cache.needle.isEmpty())
    {
        const QString text = cache.needle;
        const bool wildcards = cache.wildcards;
        const bool regularExpressions = cache.regularExpressions;
        // Soft-invalidate: drop the cache so `UpdateFindMatchCount`
        // rescans, but leave the rail alone — the recount replaces
        // ticks in one shot.
        findCacheOpt.reset();
        UpdateFindMatchCount(text, wildcards, regularExpressions);
        return;
    }

    // Empty needle: best-effort restore from the capped row list.
    mOverviewRailModel->SetMatchProxyRows(cache.sortedRows);
}

void MainWindow::OnFindCacheInvalidated()
{
    InvalidateFindMatchCache();
    if (mFindRecord != nullptr && IsFindBarVisible())
    {
        mFindRecord->BumpMatchCountDebounce();
    }
}

void MainWindow::OnEnumColumnsChangedApplyFilterRebuild(EnumColumnsChangeReason reason, int columnIndex)
{
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
    if (mModel == nullptr || mSortFilterProxyModel == nullptr || mSession == nullptr)
    {
        return;
    }
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
            if (const auto *levelMapping = mModel->LastBatchLevelDemoteMappingFor(columnIndex); levelMapping != nullptr)
            {
                // Re-entrancy guard: the rewrite + downstream sync
                // calls walk `mSimpleLeaves`, so a transitive re-emit
                // of `enumColumnsChanged` for the same column would
                // see half-rewritten state.
                if (mSession->IsApplyingEnumRebuild())
                {
                    return;
                }
                mSession->SetApplyingEnumRebuild(true);
                const auto demoteGuard = qScopeGuard([this]() { mSession->SetApplyingEnumRebuild(false); });
                const auto &columnsCfg = mModel->Configuration().columns;
                const loglib::LogConfiguration::Column *demotedColumn =
                    std::cmp_less(columnIndex, columnsCfg.size()) ? &columnsCfg[static_cast<size_t>(columnIndex)]
                                                                  : nullptr;
                for (auto &kv : mSession->MutableSimpleLeaves())
                {
                    loglib::LeafRule &filter = kv.second;
                    const int resolvedRow = ResolveLeafColumnByKeys(filter.columnKeys, columnsCfg);
                    if (resolvedRow != columnIndex)
                    {
                        continue;
                    }
                    if (filter.type != loglib::LeafRule::Type::Enumeration)
                    {
                        continue;
                    }
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
                    std::ranges::sort(expanded);
                    const auto dupTail = std::ranges::unique(expanded);
                    expanded.erase(dupTail.begin(), dupTail.end());
                    filter.filterValues = std::move(expanded);
                }
                MirrorSessionStateToConfiguration();
                SyncColumnFilterIndicators();
            }
        }
    }
    // `columnIndex == -1` means "scope unknown" -- treat as
    // matches-anything to keep the safe broad behaviour.
    const auto &columnsForResolve = mModel->Configuration().columns;
    const auto matchesAffectedColumn = [columnIndex, &columnsForResolve](const auto &kv) {
        if (columnIndex < 0)
        {
            return true;
        }
        return ResolveLeafColumnByKeys(kv.second.columnKeys, columnsForResolve) == columnIndex;
    };
    bool rebuild = false;
    switch (reason)
    {
    case EnumColumnsChangeReason::Demoted:
        rebuild = std::ranges::any_of(Filters(), [&matchesAffectedColumn](const auto &kv) {
            return kv.second.type == loglib::LeafRule::Type::Enumeration && matchesAffectedColumn(kv);
        });
        break;
    case EnumColumnsChangeReason::Promoted:
        rebuild = std::ranges::any_of(Filters(), [&matchesAffectedColumn](const auto &kv) {
            return kv.second.type == loglib::LeafRule::Type::Enumeration && matchesAffectedColumn(kv);
        });
        break;
    case EnumColumnsChangeReason::Grew:
        rebuild = std::ranges::any_of(Filters(), [this, &matchesAffectedColumn](const auto &kv) {
            return kv.second.type == loglib::LeafRule::Type::Enumeration && matchesAffectedColumn(kv) &&
                   !EnumFilterFullyResolved(kv.second);
        });
        break;
    }
    if (rebuild)
    {
        // Re-entrancy guard: an inner `UpdateFilters` that re-emits
        // `enumColumnsChanged` must not rebuild on a half-updated
        // state. Queued signals that arrive after the outer call
        // returns rebuild normally.
        if (mSession->IsApplyingEnumRebuild())
        {
            return;
        }
        mSession->SetApplyingEnumRebuild(true);
        const auto guard = qScopeGuard([this]() { mSession->SetApplyingEnumRebuild(false); });
        UpdateFilters();
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
        mSortFilterProxyModel->SetFilterExpression(loglib::CompiledFilterExpression{});
        mTableView->sortByColumn(-1, Qt::AscendingOrder);

        // Cancel export and decompression before the reset: workers
        // must not continue against a cleared model.
        CancelInFlightExport();
        CancelInFlightDecompression();

        // See `NewSession` for the session-switch latch rationale.
        const SessionSwitchScope switchGuard(*this);

        mModel->Reset();
        // Config load is a session boundary; re-arm the auto-raise
        // and reset the per-file slice watermark.
        if (mParseErrorsDock != nullptr)
        {
            mParseErrorsDock->ResetSessionState();
        }
        // Fully quiesce the outgoing session before applying the
        // new configuration -- mirrors `NewSession`. Without this,
        // queued-but-not-yet-drained files leak into the new
        // session and the toolbar / streaming counters keep
        // displaying the old session's state. The
        // `mSessionMode = Idle` + `SetConfigurationUiEnabled(true)`
        // pair also re-enables toolbar controls a live-tail
        // session might have greyed out. If the caller is about
        // to start streaming from the loaded config, its dispatch
        // path re-flips these.
        mSession->ClearPendingOpenQueues();
        // Pending promotion and Undo state belong to the replaced session.
        ClearPendingLiveTailPromotion();
        ClearRotationExpansionUndoState();
        mSession->ResetMode();
        mSession->ResetStreamingCountersAndFileName();
        // Session boundary: drop the Goto Timestamp / Goto Line
        // sticky inputs so a value that made sense in the outgoing
        // session (zone, format, row count) does not pre-populate
        // the dialog in the new one. `NewSession` already does the
        // same via `mSessionView`; the File -> Load Configuration
        // path also goes through here.
        // Guard on `mSessionView` for symmetry with `NewSession`
        // -- construction never leaves the view null in
        // production but the guard costs nothing.
        if (mSessionView != nullptr)
        {
            mSessionView->ClearGotoStickyInputs();
        }
        SetConfigurationUiEnabled(true);
        UpdateStreamToolbarVisibility();
        UpdateStreamingStatus();
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
        // *Exception*: when the session's deferred-sort latch is
        // set (streaming will follow), skip the eager apply -- see
        // `ApplyDeferredSortFromConfig` for the O(N^2) avoidance.
        if (!mSession->HasPendingApplySortFromConfig())
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
        mSession->MutableCurrentSource() = mModel->Configuration().source;
        logapp::BackfillLocatorDedupKeys(mSession->MutableCurrentSource());
        // Session loading updates the menu but does not clear this
        // window's CLI opt-out; an explicit menu change does.
        SyncRotationHistoryActionCheckedState();

        // Bulk-replace anchors from the loaded vector. Newer-schema
        // colour slots are clamped (not dropped) so bookmark
        // positions + notes survive a downgrade; the count is
        // surfaced to the user.
        if (mAnchors != nullptr)
        {
            const std::size_t clampedAnchorCount = mAnchors->Replace(mModel->Configuration().anchors);
            if (clampedAnchorCount > 0)
            {
                statusBar()->showMessage(
                    tr("%1 anchor(s) from a newer schema had their colour clamped to slot %2.")
                        .arg(static_cast<qulonglong>(clampedAnchorCount))
                        .arg(static_cast<qulonglong>(loglib::ANCHOR_PALETTE_SIZE)),
                    STATUS_BAR_MESSAGE_TIMEOUT_MS
                );
            }
        }

        RebuildFiltersFromConfiguration();

        // Install loaded highlight rules against the current
        // columns. The table is empty (reset above wiped rows), so
        // the row-match cache seeds empty; the wired `rowsInserted`
        // hook fills it as streams populate.
        if (mHighlights != nullptr)
        {
            const auto &config = mModel->Configuration();
            mHighlights->SetRules(config.highlightRules, config.columns, &mModel->Table());
            const std::size_t inactive = mHighlights->InactiveCount();
            if (inactive > 0)
            {
                statusBar()->showMessage(
                    tr("%1 highlight rule(s) inactive against the loaded columns.")
                        .arg(static_cast<qulonglong>(inactive)),
                    STATUS_BAR_MESSAGE_TIMEOUT_MS
                );
            }
            // Refresh any open editor so it shows the loaded rules,
            // not the pre-load buffer.
            if (mHighlightRulesEditor != nullptr && mHighlightRulesEditorSession.data() == mSession)
            {
                mHighlightRulesEditor->SetColumns(config.columns);
                mHighlightRulesEditor->SetRules(config.highlightRules);
            }
            if (mSession != nullptr)
            {
                mSession->ClearHighlightEditorDraft();
            }
        }
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
    RebuildFiltersFromConfiguration(mSession);
}

void MainWindow::RebuildFiltersFromConfiguration(LogSession *session)
{
    if (session == nullptr)
    {
        return;
    }
    LogModel *const model = session->Model();
    if (model == nullptr)
    {
        return;
    }
    const bool sessionIsActive = session == mSession;

    // Extract the simple-mode subset (top-level `Leaf` children of
    // the root `And`, or a bare-`Leaf` root) from the loaded
    // expression and re-add each via `AddLogFilter`. Non-Leaf
    // children and non-`And` roots are Advanced clauses and are
    // preserved by `MirrorSessionStateToConfiguration`. UUIDs are
    // GUI-only and regenerated here.
    std::vector<loglib::LeafRule> loadedFilters;
    const auto &loadedExpression = model->Configuration().expression;
    if (const auto *rootAnd = std::get_if<loglib::FilterExpression::And>(&loadedExpression.node); rootAnd != nullptr)
    {
        loadedFilters.reserve(rootAnd->children.size());
        for (const auto &child : rootAnd->children)
        {
            if (const auto *leaf = std::get_if<loglib::FilterExpression::Leaf>(&child.node); leaf != nullptr)
            {
                loadedFilters.push_back(leaf->rule);
            }
        }
    }
    else if (const auto *leaf = std::get_if<loglib::FilterExpression::Leaf>(&loadedExpression.node); leaf != nullptr)
    {
        loadedFilters.push_back(leaf->rule);
    }

    // Suppress per-filter dirty/title updates; emit one consolidated state
    // on scope exit. The session owns both flags; the
    // `filtersDirtyChanged` signal only fires on a real transition,
    // so we always force `UpdateWindowTitle()` after the load so a
    // clean-to-clean load still refreshes the title (source path
    // changed, tab label needs redraw, etc.).
    session->SetLoadingConfiguration(true);
    const auto guard = qScopeGuard([this, session, sessionIsActive]() {
        session->SetLoadingConfiguration(false);
        // Loaded set matches disk, so start clean.
        session->ClearFiltersDirty();
        if (sessionIsActive)
        {
            UpdateWindowTitle();
        }
    });

    // Reset simple-mode surface only; keep the loaded expression on
    // the manager so Mirror preserves any Advanced subtree.
    // (`ClearAllFilters` would drop the whole expression -- see
    // `TestClearAllFiltersDropsAdvancedTree`.)
    ResetSimpleFilterState(session);
#ifdef LOGAPP_BUILD_TESTING
    mLastDroppedFilterCountForTest = 0;
#endif

    const auto &columns = model->Configuration().columns;
    std::vector<FilterValidationFailure> dropped;
    for (const auto &saved : loadedFilters)
    {
        if (auto failure = ValidateFilterAgainstColumns(saved, columns))
        {
            dropped.push_back(std::move(*failure));
            continue;
        }
        // Defer mirror + rule rebuild; one trailing sync below.
        AddLogFilter(session, QUuid::createUuid().toString(), saved, /*deferSync=*/true);
    }
    MirrorSessionStateToConfiguration(session);
    UpdateFilters(session);
    // Re-enable Clear-All for the Advanced-only / all-leaves-dropped
    // cases where no `AddLogFilter` fired but rows are still filtered.
    if (sessionIsActive)
    {
        SyncClearAllFiltersEnabled();
        SyncColumnFilterIndicators();
    }

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
        ShowDroppedFiltersDialog(static_cast<int>(dropped.size()), message, session);
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
    // Delegate view-local selection to `LogSessionView`.
    // Shell forwarder kept so existing signal connections (anchors
    // dock, histogram dock) route into the view without a rewire.
    // Status-bar message stays on the shell via the view's
    // `rowNotVisible()` signal wired in the ctor.
    if (mSessionView == nullptr)
    {
        return;
    }
    mSessionView->SelectSourceRow(sourceRow);
}

void MainWindow::GotoLine()
{
    // Delegate the view-local dialog to `LogSessionView`.
    // Shell forwarder kept so `actionGoToLine` from `main_window.ui`
    // continues to trigger the migrated dialog. Status messages
    // now flow via `LogSessionView::statusMessageRequested`, wired
    // in the ctor.
    if (mSessionView == nullptr)
    {
        return;
    }
    mSessionView->PromptGotoLine();
}

void MainWindow::ExecuteGotoLine(const QString &input)
{
    // Delegate view-local line navigation to `LogSessionView`.
    // Shell forwarder kept so `ExecuteGotoLineForTest` continues
    // to drive the range check + filter-visibility hint.
    if (mSessionView == nullptr)
    {
        return;
    }
    mSessionView->ExecuteGotoLine(input);
}

std::optional<MainWindow::GotoTimestampParse> MainWindow::ParseGotoTimestampInput(
    const QString &input, const std::vector<std::string> &columnParseFormats, std::chrono::system_clock::time_point now
)
{
    // Delegate timestamp parsing to `LogSessionView`.
    // Static forwarder kept so external callers (unit tests, other
    // shell paths) continue to compile against the historical
    // `MainWindow::ParseGotoTimestampInput` symbol. The view's
    // `GotoTimestampParse` value type is layout-compatible with
    // the shell's (identical fields).
    const auto viewParsed = LogSessionView::ParseGotoTimestampInput(input, columnParseFormats, now);
    if (!viewParsed.has_value())
    {
        return std::nullopt;
    }
    return GotoTimestampParse{.micros = viewParsed->micros, .isNaive = viewParsed->isNaive};
}

void MainWindow::GotoTimestamp()
{
    // Delegate the view-local dialog to `LogSessionView`.
    if (mSessionView == nullptr)
    {
        return;
    }
    mSessionView->PromptGotoTimestamp();
}

void MainWindow::ExecuteGotoTimestamp(const QString &input, std::chrono::system_clock::time_point now)
{
    // Delegate view-local timestamp navigation to `LogSessionView`.
    if (mSessionView == nullptr)
    {
        return;
    }
    mSessionView->ExecuteGotoTimestamp(input, now);
}

int MainWindow::FindFirstRowAtOrAfter(int timeCol, int64_t targetMicros) const
{
    return mSession->FindFirstRowAtOrAfterTimestamp(timeCol, targetMicros);
}

void MainWindow::JumpToFirstRowInBucket(std::size_t bucketIndex)
{
    if (mHistogramDock == nullptr)
    {
        return;
    }
    const HistogramModel *hm = mHistogramDock->ModelForTest();
    if (hm == nullptr)
    {
        return;
    }
    const int sourceRow = hm->FirstRowInBucket(bucketIndex);
    if (sourceRow < 0)
    {
        // Bucket exists but no live row falls in it (can happen after
        // a retention eviction before the histogram rebuild fires).
        statusBar()->showMessage(tr("No visible row in the selected bucket."), STATUS_BAR_MESSAGE_TIMEOUT_MS);
        return;
    }
    SelectSourceRow(sourceRow);
}

void MainWindow::ScrollToProxyRow(int proxyRow, bool replaceSelection)
{
    // Delegate view-local scrolling to `LogSessionView`.
    // The shell's `actionFollowTail` uncheck is now driven by the
    // view's `followTailDisengageRequested()` signal wired in the
    // ctor. Shell forwarder kept so overview-rail-widget's
    // `proxyRowClicked` connection continues to work unchanged.
    if (mSessionView == nullptr)
    {
        return;
    }
    mSessionView->ScrollToProxyRow(proxyRow, replaceSelection);
}

void MainWindow::SetOverviewRailVisible(bool visible)
{
    if (mTableView == nullptr || mOverviewRailWidget == nullptr)
    {
        return;
    }
    // Persist immediately so the next launch honours the current
    // preference. Guarded to skip the redundant write on the
    // load-time seed (replaying the persisted value verbatim) —
    // avoids a Windows registry write on every window construction.
    QSettings settings;
    if (settings.value(QStringLiteral("ui/showOverviewRail"), true).toBool() != visible)
    {
        settings.setValue(QStringLiteral("ui/showOverviewRail"), visible);
    }

    // Mirror the QAction check state for programmatic callers;
    // block the signal so this doesn't re-enter us.
    if (mActionToggleOverviewRail != nullptr && mActionToggleOverviewRail->isChecked() != visible)
    {
        const QSignalBlocker blocker(mActionToggleOverviewRail);
        mActionToggleOverviewRail->setChecked(visible);
    }

    if (visible)
    {
        // Attach reparents + shows the widget; its `resizeEvent`
        // then calls `SetBucketCount(H)` on the model (synchronous
        // rebuild), so we don't need `Rebuild()` here.
        mTableView->AttachOverviewRail(mOverviewRailWidget);
        // Re-push match ticks only while find is visible — same
        // contract as the find-dock hide handlers. Same-H hide→show
        // restores via durable model state; a height change (or a
        // scan that ran while the rail was hidden) needs the
        // re-bucket / rescan path in `PushFindMatchesToOverviewRail`.
        if (IsFindBarVisible())
        {
            PushFindMatchesToOverviewRail();
        }
    }
    else
    {
        mTableView->AttachOverviewRail(nullptr);
        // Reparent to the owning `LogSessionView` so the widget survives detach without being orphaned
        // onto the shell. `AttachOverviewRail(null)` already
        // dropped the parent -- if we reparent onto `this`
        // (MainWindow) then destroying `mSessionView` leaves a
        // dangling rail widget on the shell's child list. Falling
        // back to `this` preserves behavior when
        // `mSessionView` is absent (e.g. torn down mid-teardown).
        if (mSessionView != nullptr)
        {
            mOverviewRailWidget->setParent(mSessionView);
        }
        else
        {
            mOverviewRailWidget->setParent(this);
        }
        mOverviewRailWidget->hide();
        // Drop the bucket vector so `RebuildInternal` short-circuits
        // on incoming proxy signals while the rail is hidden.
        // `SetBucketCount(H)` on the next `showEvent` re-arms it.
        if (mOverviewRailModel != nullptr)
        {
            mOverviewRailModel->SetBucketCount(0);
        }
    }
}

void MainWindow::AddTimeRangeFilterFromHistogram(qint64 fromEpochMicros, qint64 toEpochMicros)
{
    if (mHistogramDock == nullptr || mModel == nullptr)
    {
        return;
    }
    const HistogramModel *hm = mHistogramDock->ModelForTest();
    if (hm == nullptr || !hm->HasTimeColumn())
    {
        // Hard gate against an out-of-band signal installing a filter
        // on a non-time column (the widget also guards, but be safe).
        statusBar()->showMessage(
            tr("Cannot filter by time \u2014 this log has no time column."), STATUS_BAR_MESSAGE_TIMEOUT_MS
        );
        return;
    }
    if (fromEpochMicros > toEpochMicros)
    {
        std::swap(fromEpochMicros, toEpochMicros);
    }
    const int column = hm->TimeColumnIndex();
    // Sentinel filter ID so a second drag replaces the previous
    // histogram range rather than stacking a duplicate filter.
    static const QString HISTOGRAM_FILTER_ID = QStringLiteral("histogram-time-range");
    FilterTimeStampSubmitted(HISTOGRAM_FILTER_ID, column, fromEpochMicros, toEpochMicros);
}

void MainWindow::JumpToAnchor(bool forward)
{
    // Delegate view-local anchor navigation to `LogSessionView`.
    // Shell forwarder kept so `mActionJumpNextAnchor` /
    // `mActionJumpPrevAnchor` continue to trigger the migrated
    // navigation. Status feedback flows via the view's
    // `statusMessageRequested` signal.
    if (mSessionView == nullptr)
    {
        return;
    }
    mSessionView->JumpToAnchor(forward);
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
    const QString &filterId, const std::optional<loglib::LeafRule> &filter, bool openEditor, int initialColumn
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
    // this pre-guard adapts its result to the existing status-bar UX.
    // The post-editor "missing payload" guards remain inline because
    // they need to delete the editor on failure.
    std::optional<loglib::LeafRule> resolvedFilter = filter;
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
                // re-reads the live `mSimpleLeaves`). Guard anyway: a stale
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
        // Configuration-load path: filter is already in `mSimpleLeaves`.
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
        // Column keys are the wire identity; the editor needs a
        // live column index, so resolve once here and forward.
        const int editorRow = ResolveLeafColumnByKeys(resolvedFilter->columnKeys, mModel->Configuration().columns);
        if (resolvedFilter->type == loglib::LeafRule::Type::Time)
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
            filterEditor->Load(editorRow, begin, end);
        }
        else if (resolvedFilter->type == loglib::LeafRule::Type::Enumeration)
        {
            QStringList values;
            values.reserve(static_cast<qsizetype>(resolvedFilter->filterValues.size()));
            for (const std::string &v : resolvedFilter->filterValues)
            {
                values.append(QString::fromStdString(v));
            }
            filterEditor->Load(editorRow, values);
        }
        else if (resolvedFilter->type == loglib::LeafRule::Type::Number)
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
            filterEditor->Load(editorRow, resolvedFilter->filterMinValue, resolvedFilter->filterMaxValue);
        }
        else if (resolvedFilter->type == loglib::LeafRule::Type::Boolean)
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
            filterEditor->Load(editorRow, sides.includeTrue, sides.includeFalse);
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
                editorRow,
                QString::fromStdString(*resolvedFilter->filterString),
                static_cast<int>(*resolvedFilter->matchType)
            );
        }
    }
    filterEditor->show();
}

const std::unordered_map<std::string, loglib::LeafRule> &MainWindow::Filters() const
{
    return mSession->SimpleLeaves();
}

void MainWindow::ResetSimpleFilterState()
{
    ResetSimpleFilterState(mSession);
}

void MainWindow::ResetSimpleFilterState(LogSession *session)
{
    if (session == nullptr)
    {
        return;
    }
    session->ResetSimpleFilterState();
    if (session != mSession || ui == nullptr || ui->menuFilters == nullptr)
    {
        return;
    }
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

void MainWindow::SyncClearAllFiltersEnabled()
{
    if (ui == nullptr || ui->actionClearAllFilters == nullptr)
    {
        return;
    }
    ui->actionClearAllFilters->setDisabled(loglib::IsMatchAll(mModel->Configuration().expression));
    // Status-bar button gates on the same condition via
    // `UpdateRowsShownStatus`; route through it so the two stay in
    // lockstep on filter changes that don't shift row counts.
    UpdateRowsShownStatus();
}

void MainWindow::ClearAllFilters()
{
    ResetSimpleFilterState();
    // Reset the full expression before mirroring: the user asked
    // for a clean slate, so drop any Advanced-mode tree too. The
    // mirror otherwise preserves non-Leaf Advanced subtrees, which
    // would leave filtering active with no UI cue.
    mModel->ConfigurationManager().SetExpression(loglib::FilterExpression{});
    MirrorSessionStateToConfiguration();
    UpdateFilters();
    MarkFiltersDirty();
    SyncColumnFilterIndicators();
}

void MainWindow::OpenAdvancedFilter()
{
    // Seed from the live configuration so simple-mode leaves
    // round-trip through the pretty-printer.
    AdvancedFilterEditor editor(this);
    editor.LoadFromExpression(mModel->Configuration().expression);
    if (editor.exec() != QDialog::Accepted)
    {
        return;
    }
    auto result = editor.Result();
    if (!result.has_value())
    {
        // Defensive: the OK button disables on parse error, but
        // don't crash if that invariant is ever broken.
        return;
    }
    ApplyAdvancedFilterResult(std::move(*result));
}

void MainWindow::ApplyAdvancedFilterResult(loglib::FilterExpression result)
{
    // Split the parsed tree into (simple leaves, non-Leaf remainder)
    // so it obeys the same shape as the load path:
    //
    //   expression = And([mSimpleLeaves...], non-Leaf remainder...)
    //
    // Extracting the simple leaves lets the Filters menu show one
    // entry per leaf (so Edit/Remove keep working post-Advanced),
    // and prevents `MirrorSessionStateToConfiguration` from later
    // dropping a bare `Leaf` root under the "already in simple"
    // rule.
    ResetSimpleFilterState();

    const auto &columns = mModel->Configuration().columns;
    // Only leaves the simple editor can actually load belong in
    // `mSimpleLeaves`: `ValidateFilterAgainstColumns` is what
    // `FilterEditor` uses. The grammar accepts looser leaves
    // (e.g. `level:error` as String vs. an Enumeration column) --
    // those need to stay Advanced so the next load doesn't drop
    // them as a column-type mismatch.
    const auto isSimpleRepresentable = [&columns](const loglib::LeafRule &rule) {
        return !ValidateFilterAgainstColumns(rule, columns).has_value();
    };

    // The query parser records exactly one key on each leaf (the
    // typed column name). Real columns often carry aliases -- e.g.
    // a Level column with `keys = {"level", "severity", "lvl"}` --
    // and simple-mode leaves built via the FilterEditor path bind
    // the full alias vector so the rule survives rename / alias-drop
    // edits later. Promote the parser's single-key binding to the
    // resolved column's full `keys` vector when extracting into
    // `mSimpleLeaves`; safe because `isSimpleRepresentable` already
    // proved the resolve.
    const auto promoteToColumnKeys = [&columns](loglib::LeafRule rule) {
        const int resolved = ResolveLeafColumnByKeys(rule.columnKeys, columns);
        if (resolved >= 0)
        {
            rule.columnKeys = columns[static_cast<std::size_t>(resolved)].keys;
        }
        return rule;
    };

    std::vector<loglib::LeafRule> extractedLeaves;
    loglib::FilterExpression::And rest;
    // Wrap a bare `Leaf` remainder in a single-child `And`:
    // `MirrorSessionStateToConfiguration` drops top-level Leaves
    // (they should live in `mSimpleLeaves`), so unwrapped it would
    // silently vanish on the next mirror. The wrap is a no-op for
    // evaluation and formatting.
    const auto keepAsAdvanced = [&rest](loglib::FilterExpression node) {
        if (std::holds_alternative<loglib::FilterExpression::Leaf>(node.node))
        {
            std::vector<loglib::FilterExpression> wrapped;
            wrapped.push_back(std::move(node));
            rest.children.push_back(loglib::MakeAnd(std::move(wrapped)));
            return;
        }
        rest.children.push_back(std::move(node));
    };

    if (const auto *rootLeaf = std::get_if<loglib::FilterExpression::Leaf>(&result.node); rootLeaf != nullptr)
    {
        if (isSimpleRepresentable(rootLeaf->rule))
        {
            extractedLeaves.push_back(promoteToColumnKeys(rootLeaf->rule));
        }
        else
        {
            keepAsAdvanced(std::move(result));
        }
    }
    else if (auto *rootAnd = std::get_if<loglib::FilterExpression::And>(&result.node); rootAnd != nullptr)
    {
        rest.children.reserve(rootAnd->children.size());
        for (auto &child : rootAnd->children)
        {
            const auto *leaf = std::get_if<loglib::FilterExpression::Leaf>(&child.node);
            if (leaf != nullptr && isSimpleRepresentable(leaf->rule))
            {
                extractedLeaves.push_back(promoteToColumnKeys(leaf->rule));
            }
            else
            {
                keepAsAdvanced(std::move(child));
            }
        }
    }
    else
    {
        // Bare `Or` / `Not`: preserved verbatim.
        keepAsAdvanced(std::move(result));
    }
    loglib::FilterExpression remainder;
    remainder.node = std::move(rest);

    // Install the non-Leaf remainder first so the mirror preserves
    // it, then re-add the extracted leaves through `AddLogFilter`
    // with deferred sync (one Mirror at the end).
    mModel->ConfigurationManager().SetExpression(std::move(remainder));
    for (const auto &leaf : extractedLeaves)
    {
        AddLogFilter(QUuid::createUuid().toString(), leaf, /*deferSync=*/true);
    }
    MirrorSessionStateToConfiguration();
    UpdateFilters();
    SyncClearAllFiltersEnabled();
    MarkFiltersDirty();
    SyncColumnFilterIndicators();
}

void MainWindow::ClearFilter(const QString &filterID, bool deferSync)
{
    const std::string idKey = filterID.toStdString();
    mSession->MutableSimpleLeaves().erase(idKey);
    auto &order = mSession->MutableSimpleLeafOrder();
    if (const auto it = std::ranges::find(order, idKey); it != order.end())
    {
        order.erase(it);
    }
    if (!deferSync)
    {
        MirrorSessionStateToConfiguration();
        UpdateFilters();
    }
    MarkFiltersDirty();

    if (ui != nullptr && ui->menuFilters != nullptr)
    {
        for (QAction *action : ui->menuFilters->actions())
        {
            if (!action->data().toString().isNull() && action->data().toString() == filterID)
            {
                ui->menuFilters->removeAction(action);
                delete action;
            }
        }
    }

    // Only sync when Mirror has just published a fresh expression;
    // deferSync paths always follow up with `AddLogFilter` which
    // re-enables the action.
    if (!deferSync)
    {
        SyncClearAllFiltersEnabled();
        SyncColumnFilterIndicators();
    }
}

void MainWindow::FilterSubmitted(const QString &filterID, int row, const QString &filterString, int matchType)
{
    const auto match = static_cast<loglib::LeafRule::Match>(matchType);

    // Reject an invalid regex up front; the downstream
    // `QRegularExpression` would otherwise compile to an invalid
    // object and silently hide every row. Wildcards always compile.
    if (match == loglib::LeafRule::Match::RegularExpression)
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

    loglib::LeafRule filter;
    filter.type = loglib::LeafRule::Type::String;
    filter.columnKeys = ColumnKeysForRow(row, mModel->Configuration().columns);
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

    loglib::LeafRule filter;
    filter.type = loglib::LeafRule::Type::Time;
    filter.columnKeys = ColumnKeysForRow(row, mModel->Configuration().columns);
    filter.filterBegin = beginTimeStamp;
    filter.filterEnd = endTimeStamp;

    AddLogFilter(filterID, filter);
}

void MainWindow::FilterEnumSubmitted(const QString &filterID, int row, const QStringList &selectedValues)
{
    ClearFilter(filterID, /*deferSync=*/true);

    loglib::LeafRule filter;
    filter.type = loglib::LeafRule::Type::Enumeration;
    filter.columnKeys = ColumnKeysForRow(row, mModel->Configuration().columns);
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

    loglib::LeafRule filter;
    filter.type = loglib::LeafRule::Type::Number;
    filter.columnKeys = ColumnKeysForRow(row, mModel->Configuration().columns);
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

    loglib::LeafRule filter;
    filter.type = loglib::LeafRule::Type::Boolean;
    filter.columnKeys = ColumnKeysForRow(row, mModel->Configuration().columns);
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

QString MainWindow::BuildFilterTitle(const loglib::LeafRule &filter) const
{
    // No `default:`: a new `LeafRule::Type` must trip `-Wswitch`
    // rather than silently fall through and deref a `nullopt`.
    //
    // Every leaf in `mSimpleLeaves` is supposed to carry a fully
    // populated payload -- `ValidateFilterAgainstColumns` gates both
    // the load path and the Advanced-commit path. An empty payload
    // therefore means something upstream is wrong, but this is a
    // *display* helper reached while building menus and tooltips, so
    // it renders a visible placeholder rather than asserting: a
    // hand-edited config should not take the Debug build down, and
    // "(unset)" in the Filters menu points at the problem far better
    // than a crash. (The `Time` case has always worked this way; the
    // other branches used to `Q_ASSERT`.) The placeholder is spelled
    // out at each site rather than hoisted into a local so the return
    // can move it; `lupdate` folds the identical literals into one
    // translatable entry.
    switch (filter.type)
    {
    case loglib::LeafRule::Type::Time:
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
    case loglib::LeafRule::Type::Enumeration:
    {
        if (filter.filterValues.empty())
        {
            return tr("(unset)");
        }
        QStringList values;
        values.reserve(static_cast<qsizetype>(filter.filterValues.size()));
        for (const std::string &v : filter.filterValues)
        {
            values.append(QString::fromStdString(v));
        }
        return values.join(QStringLiteral(", "));
    }
    case loglib::LeafRule::Type::Number:
    {
        // Unbounded on both sides renders as `[-inf, +inf]` below,
        // which reads as "matches anything" -- accurate, and the same
        // treatment the `Time` case gives `any - any`.
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
    case loglib::LeafRule::Type::Boolean:
    {
        // Canonicalise to "true, false" order regardless of how
        // `filter.filterValues` is laid out (the submit slot always
        // writes "true" first, but a hand-edited config might not).
        const BooleanFilterSides sides = DecodeBooleanFilterSides(filter.filterValues);
        if (!sides.includeTrue && !sides.includeFalse)
        {
            return tr("(unset)");
        }
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
    case loglib::LeafRule::Type::String:
        if (!filter.filterString.has_value())
        {
            return tr("(unset)");
        }
        // `Exactly ""` is a valid query that matches genuinely empty
        // values. Keep it distinct from a missing payload in menus and
        // column tooltips. Other empty string match kinds are rejected by
        // validation before they can reach the simple-mode surface.
        if (filter.filterString->empty())
        {
            return filter.matchType == loglib::LeafRule::Match::Exactly ? QStringLiteral("\"\"") : tr("(unset)");
        }
        return QString::fromStdString(*filter.filterString);
    }
    Q_ASSERT_X(false, "MainWindow::BuildFilterTitle", "unhandled LeafRule::Type");
    return tr("(unset)");
}

void MainWindow::AddLogFilter(const QString &id, const loglib::LeafRule &filter, bool deferSync)
{
    AddLogFilter(mSession, id, filter, deferSync);
}

void MainWindow::AddLogFilter(LogSession *session, const QString &id, const loglib::LeafRule &filter, bool deferSync)
{
    if (session == nullptr)
    {
        return;
    }
    const std::string idKey = id.toStdString();
    // Preserve insertion order in `mSimpleLeafOrder` so the mirror
    // step can rebuild the top-level `And` deterministically.
    // Update-in-place edits (Edit -> OK re-adds under the same
    // UUID) shouldn't append a second entry; the linear scan is
    // cheap for realistic filter counts.
    auto &order = session->MutableSimpleLeafOrder();
    if (std::ranges::find(order, idKey) == order.end())
    {
        order.push_back(idKey);
    }
    session->MutableSimpleLeaves()[idKey] = filter;
    if (!deferSync)
    {
        MirrorSessionStateToConfiguration(session);
        UpdateFilters(session);
    }
    // Every user-driven filter mutation funnels through here, so one
    // mark-dirty covers them all. Config reloads are silenced by the guard.
    session->MarkFiltersDirty();

    if (session != mSession || ui == nullptr || ui->menuFilters == nullptr)
    {
        return;
    }
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
    // Lint suppression: `mSimpleLeaves.find` and `LeafRule` copy
    // can technically throw, but the body has no real source of
    // exceptions. Same for the matching lambda in
    // `BuildHeaderContextMenu`.
    // NOLINTNEXTLINE(bugprone-exception-escape)
    connect(editAction, &QAction::triggered, this, [this, id]() {
        const auto it = Filters().find(id.toStdString());
        if (it == Filters().end())
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
        const auto &columns = mModel->Configuration().columns;
        for (const auto &[id, filter] : Filters())
        {
            const int row = ResolveLeafColumnByKeys(filter.columnKeys, columns);
            if (row < 0 || row >= cols)
            {
                // Column keys don't resolve (dropped/renamed); hide
                // the tooltip entry until the column reappears.
                continue;
            }
            perColumnTitles[static_cast<size_t>(row)].append(BuildFilterTitle(filter));
        }
        // Sort titles for stable tooltip ordering. `QCollator` gives
        // locale-aware case-insensitive numeric ordering (`9` < `10`).
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
    ApplyDeferredSortFromConfig(mSession, mSessionView.data());
}

void MainWindow::ApplyDeferredSortFromConfig(LogSession *session, LogSessionView *view)
{
    if (session == nullptr)
    {
        return;
    }
    LogFilterModel *const filter = session->FilterProxy();
    LogModel *const model = session->Model();
    LogTableView *const table = view != nullptr ? view->TableView() : nullptr;
    if (filter == nullptr || model == nullptr || table == nullptr)
    {
        session->SetPendingApplySortFromConfig(false);
        return;
    }
    // Always clear the latch so subsequent saves read the proxy's
    // live sort instead of preserving the loaded one.
    const auto guard = qScopeGuard([session]() { session->SetPendingApplySortFromConfig(false); });
    if (!session->HasPendingApplySortFromConfig())
    {
        return;
    }
    // User sorted mid-stream -- their choice wins.
    if (filter->SortColumn() >= 0)
    {
        return;
    }
    const auto &cfgSort = model->Configuration().sort;
    if (cfgSort.columnIndex < 0 || cfgSort.columnIndex >= model->columnCount())
    {
        return;
    }
    table->sortByColumn(cfgSort.columnIndex, cfgSort.descending ? Qt::DescendingOrder : Qt::AscendingOrder);
}

bool MainWindow::EnumFilterFullyResolved(const loglib::LeafRule &filter) const
{
    if (filter.type != loglib::LeafRule::Type::Enumeration)
    {
        return true;
    }
    const auto &columnsCfg = mModel->Configuration().columns;
    const int resolvedRow = ResolveLeafColumnByKeys(filter.columnKeys, columnsCfg);
    if (resolvedRow < 0)
    {
        // Column keys don't resolve; leaf is inert and doesn't
        // need rebuilding either.
        return true;
    }
    const loglib::EnumDictionary *dictionary = ResolveEnumDictionary(resolvedRow);
    if (dictionary == nullptr)
    {
        // Column not yet promoted: defer resolution until first growth.
        return false;
    }
    // Level columns hold canonical names in `filter.filterValues` and
    // expand them to raw entries at predicate-build time. Dictionary
    // growth can surface entries matching a selected level, so treat
    // these as never fully resolved and rebuild on every `Grew`.
    if (columnsCfg[static_cast<size_t>(resolvedRow)].type == loglib::LogConfiguration::Type::Level)
    {
        return false;
    }
    return std::ranges::all_of(filter.filterValues, [dictionary](const std::string &value) {
        return dictionary->Find(value) != loglib::INVALID_ENUM_VALUE_ID;
    });
}

void MainWindow::UpdateFilters()
{
    UpdateFilters(mSession);
}

void MainWindow::UpdateFilters(LogSession *session)
{
    if (session == nullptr)
    {
        return;
    }
    LogModel *const model = session->Model();
    LogFilterModel *const filter = session->FilterProxy();
    if (model == nullptr || filter == nullptr)
    {
        return;
    }
    // Compile the mirrored `FilterExpression` into a
    // `CompiledFilterExpression` via the shared factory (handles
    // predicate construction, level-column expansion, and
    // cost-based ordering).
    loglib::CompiledFilterExpression compiled =
        CompileExpression(model->Configuration().expression, model->Configuration().columns, &model->Table());
    filter->SetFilterExpression(std::move(compiled));
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
        // slot re-applies visibility and recompiles the filter
        // expression (leaves bind by `columnKeys`, no remap needed).
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
    // carries parent fields for compatibility; ignore defensively.
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
    // Filters and highlight rules both bind by column keys, so the
    // stored payload survives the move unchanged. The compiled
    // expression, however, caches resolved column indices, so it
    // must be rebuilt against the freshly rotated column vector.
    (void)src;
    (void)finalDest;
    UpdateFilters();
    // Highlight rules bind by keys, but the compiled predicates
    // cache resolved column *indices*. `MoveColumn` rotates the
    // column vector, so refresh both caches or highlights would
    // start tinting rows against the wrong field.
    if (mHighlights != nullptr && mModel != nullptr)
    {
        mHighlights->RebindColumns(mModel->Configuration().columns, &mModel->Table());
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
    // `mSimpleLeaves` is an unordered_map keyed by UUID, so without
    // sorting the menu order would be effectively random.
    struct FilterEntry
    {
        std::string id;
        QString title;
        loglib::LeafRule::Type type;
    };
    std::vector<FilterEntry> filtersForColumn;
    filtersForColumn.reserve(Filters().size());
    const auto &columnsForResolve = mModel->Configuration().columns;
    for (const auto &entry : Filters())
    {
        if (ResolveLeafColumnByKeys(entry.second.columnKeys, columnsForResolve) == logicalColumn)
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
            const auto it = Filters().find(filterId.toStdString());
            if (it == Filters().end())
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
        // every shortcut and icon - one source of truth
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
        auto addRangeAction =
            [this, menu, &timeKeys](const QString &label, std::optional<qint64> begin, std::optional<qint64> end) {
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
    QAction *editNoteAction = anchorMenu->addAction(tr("Edit note\u2026"));
    editNoteAction->setEnabled(rightClickedKey.has_value() && currentColour.has_value());
    // Capture the key (not the row index) so a queued FIFO eviction
    // between menu build and click can't redirect the edit to a
    // different anchor at the old row slot.
    if (rightClickedKey.has_value())
    {
        // Reference-binding into the by-value capture is a workaround
        // for `performance-unnecessary-copy-initialization` on a
        // named temporary; the closure still holds an owned copy.
        const AnchorManager::Key &capturedKey = *rightClickedKey;
        // NOLINTNEXTLINE(bugprone-exception-escape) - Key's std::string capture copy can technically throw bad_alloc.
        connect(editNoteAction, &QAction::triggered, this, [this, capturedKey]() {
            EditAnchorNoteForKey(capturedKey);
        });
    }

    QAction *clearAction = anchorMenu->addAction(tr("Remove anchor"));
    clearAction->setEnabled(rightClickedKey.has_value() && currentColour.has_value());
    connect(clearAction, &QAction::triggered, mTableView, &LogTableView::ClearAnchorOnSelection);
}

void MainWindow::EditAnchorNoteForKey(const AnchorManager::Key &key)
{
    if (mAnchors == nullptr)
    {
        return;
    }
    if (!mAnchors->ColorFor(key).has_value())
    {
        // Stale key (row-menu after FIFO eviction) or the user
        // cleared the anchor via `Ctrl+0` before the trigger fired.
        statusBar()->showMessage(tr("Row is not anchored."), STATUS_BAR_MESSAGE_TIMEOUT_MS);
        return;
    }

    const auto existingNote = mAnchors->NoteFor(key).value_or(std::string{});

    // Instantiated `QInputDialog` (not the static `getText` helper)
    // so we can reach the internal line edit and pin `maxLength`.
    // Cap is in UTF-16 code units; `SanitiseNote` still enforces
    // the true UTF-8 byte cap downstream. Caps agree for ASCII.
    QInputDialog dialog(this);
    dialog.setWindowTitle(tr("Anchor note"));
    dialog.setLabelText(tr("Note for this anchor:"));
    dialog.setInputMode(QInputDialog::TextInput);
    dialog.setTextEchoMode(QLineEdit::Normal);
    dialog.setTextValue(QString::fromStdString(existingNote));
    if (auto *dialogEditor = dialog.findChild<QLineEdit *>())
    {
        dialogEditor->setMaxLength(static_cast<int>(AnchorManager::MAX_NOTE_BYTES));
    }
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    const QString newNote = dialog.textValue();

    // Re-check presence: the dialog pumps events, so a parallel
    // `Ctrl+0` / `Clear all` / streaming eviction can remove the
    // anchor while it's open. `SetAnchorNote` returns false either
    // way (identical note or gone); only report the "gone" case.
    if (!mAnchors->SetAnchorNote(key, newNote.toStdString()))
    {
        if (!mAnchors->ColorFor(key).has_value())
        {
            statusBar()->showMessage(
                tr("Anchor was removed while the note editor was open; note discarded."), STATUS_BAR_MESSAGE_TIMEOUT_MS
            );
        }
        return;
    }

    // Truncation hint: `QLineEdit::maxLength` caps code units for
    // live feedback, but a multi-byte paste can still exceed the
    // UTF-8 byte cap enforced by `SanitiseNote`. Compare committed
    // vs. stored size so a silent trim isn't invisible.
    if (const auto stored = mAnchors->NoteFor(key); stored.has_value())
    {
        const auto committedBytes = static_cast<std::size_t>(newNote.toUtf8().size());
        if (stored->size() < committedBytes)
        {
            statusBar()->showMessage(
                tr("Note truncated to %1 bytes.").arg(static_cast<qulonglong>(AnchorManager::MAX_NOTE_BYTES)),
                STATUS_BAR_MESSAGE_TIMEOUT_MS
            );
        }
    }

    // Runtime-only anchors (empty locator) aren't persisted --
    // `Entries()` drops them from the save snapshot. Warn only for
    // actual content: an all-whitespace paste sanitises to "" and
    // shouldn't trigger the hint.
    if (key.locator.empty() && !newNote.trimmed().isEmpty())
    {
        statusBar()->showMessage(
            tr("Note stored for this session -- streaming anchors are not persisted across sessions."),
            STATUS_BAR_MESSAGE_TIMEOUT_MS
        );
    }
}

void MainWindow::EditAnchorNoteForRow(int sourceRow)
{
    if (mAnchors == nullptr || mModel == nullptr)
    {
        return;
    }
    const auto key = mModel->AnchorKeyForRow(sourceRow);
    if (!key.has_value())
    {
        statusBar()->showMessage(tr("Row is not anchored."), STATUS_BAR_MESSAGE_TIMEOUT_MS);
        return;
    }
    EditAnchorNoteForKey(*key);
}

#ifdef LOGAPP_BUILD_TESTING
bool MainWindow::SubmitAnchorNoteForRowForTest(int sourceRow, const QString &note)
{
    if (mAnchors == nullptr || mModel == nullptr)
    {
        return false;
    }
    const auto key = mModel->AnchorKeyForRow(sourceRow);
    if (!key.has_value() || !mAnchors->ColorFor(*key).has_value())
    {
        statusBar()->showMessage(tr("Row is not anchored."), STATUS_BAR_MESSAGE_TIMEOUT_MS);
        return false;
    }
    // Return true iff the row was anchored; the manager sanitises
    // internally so multi-line input collapses to one line.
    mAnchors->SetAnchorNote(*key, note.toStdString());
    return true;
}

void MainWindow::ExecuteGotoLineForTest(const QString &input)
{
    // Thin forwarder so a regression in the modal-owning slot
    // cannot mask a regression in the validation branches.
    ExecuteGotoLine(input);
}

void MainWindow::ExecuteGotoTimestampForTest(const QString &input, std::chrono::system_clock::time_point now)
{
    ExecuteGotoTimestamp(input, now);
}

QString MainWindow::LastGotoTimestampInputForTest() const
{
    // Sticky input state belongs to the view.
    if (mSessionView == nullptr)
    {
        return {};
    }
    return mSessionView->LastGotoTimestampInput();
}

void MainWindow::ForceTimestampsNonMonotonicForTest()
{
    if (mModel != nullptr)
    {
        mModel->SetTimestampsMonotonicForTest(false);
    }
}
#endif

void MainWindow::EditAnchorNoteOnCurrentRow()
{
    // AnchorsDock redirect first: F4 with focus inside the dock
    // opens the dock's own inline editor. The dock's current row
    // can differ from the main table's, and popping a modal for
    // the "other" row would be jarring. Placed before the proxy
    // null check so a partially-constructed window still honours it.
    const QWidget *focused = QApplication::focusWidget();
    if (mAnchorsDock != nullptr && focused != nullptr && mAnchorsDock->isAncestorOf(focused))
    {
        mAnchorsDock->BeginEditingCurrentNote();
        return;
    }

    // Belt-and-braces text-input guard. The primary defence is the
    // F4 `ShortcutOverride` handler in `MainWindow::event()`, but
    // this slot is also reachable via `trigger()` (tests, future
    // menu placements) where no shortcut dispatch runs.
    // `QAbstractSpinBox` covers spin boxes and `QDateTimeEdit`;
    // `QComboBox` covers editable combos.
    if (focused != nullptr &&
        (qobject_cast<const QLineEdit *>(focused) != nullptr || qobject_cast<const QTextEdit *>(focused) != nullptr ||
         qobject_cast<const QPlainTextEdit *>(focused) != nullptr ||
         qobject_cast<const QAbstractSpinBox *>(focused) != nullptr ||
         qobject_cast<const QComboBox *>(focused) != nullptr))
    {
        return;
    }

    if (mTableView == nullptr || mSortFilterProxyModel == nullptr || mRowOrderProxyModel == nullptr)
    {
        return;
    }
    const QModelIndex current = mTableView->currentIndex();
    if (!current.isValid())
    {
        statusBar()->showMessage(tr("No row is currently selected."), STATUS_BAR_MESSAGE_TIMEOUT_MS);
        return;
    }
    const int sourceRow = MapProxyIndexToSourceRow(current, mSortFilterProxyModel, mRowOrderProxyModel);
    if (sourceRow < 0)
    {
        statusBar()->showMessage(tr("No row is currently selected."), STATUS_BAR_MESSAGE_TIMEOUT_MS);
        return;
    }
    EditAnchorNoteForRow(sourceRow);
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
    // Hide/show doesn't change a leaf's column keys, so this sync
    // is usually a model-side no-op. Kept for symmetry with
    // `ApplyColumnVisibility`.
    SyncColumnFilterIndicators();
}

void MainWindow::ApplyColumnVisibility()
{
    // Header visibility is applied by
    // `LogSessionView::ApplyColumnVisibility`. Shell-scoped
    // follow-ups (find-cache invalidation + filter-indicator
    // refresh) stay here because they touch the find dock and
    // filter menus which belong to shell chrome. This wrapper
    // preserves the ordering: apply header first, then invalidate
    // caches.
    if (mSessionView != nullptr)
    {
        mSessionView->ApplyColumnVisibility();
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
    // Delegate level-cell presentation to `LogSessionView`.
    // The shell keeps ownership of `mLevelCellDelegate` (per-window
    // resource, tied to the theme) and passes it into the view so
    // the view can install/detach it against its table without
    // reaching back into the shell. `mInstalledLevelDelegateColumn`
    // migrated to the view alongside.
    if (mSessionView == nullptr)
    {
        return;
    }
    mSessionView->ApplyLevelCellDelegate(mLevelCellDelegate);
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

    // Histogram dock toggle; same pattern as the other programmatic dock actions.
    if (mActionToggleHistogram != nullptr)
    {
        viewMenu->addAction(mActionToggleHistogram);
    }

    // Overview rail toggle; same pattern.
    if (mActionToggleOverviewRail != nullptr)
    {
        viewMenu->addAction(mActionToggleOverviewRail);
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
    return mSession->FindColumnIndexByKeys(keys);
}
