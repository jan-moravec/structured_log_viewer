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
#include <loglib/internal/ascii_case.hpp>
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
#include <QMenu>
#include <QMessageBox>
#include <QScopeGuard>
#include <QSignalBlocker>
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
#include <limits>
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
    const bool typesMatch =
        (filter.type == LogFilter::Type::Time && column.type == ColumnType::Time) ||
        (filter.type == LogFilter::Type::Enumeration && column.type == ColumnType::Enumeration) ||
        (filter.type == LogFilter::Type::Boolean && column.type == ColumnType::Boolean) ||
        (filter.type == LogFilter::Type::Number && isNumericColumn) ||
        (filter.type == LogFilter::Type::String && column.type != ColumnType::Time &&
         column.type != ColumnType::Enumeration && column.type != ColumnType::Boolean && !isNumericColumn);
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
        // Drop proxy rules and the sort *before* `Load` rewrites the
        // configuration: existing rules / `mSortColumn` were built
        // for the old column layout and would otherwise evaluate
        // against the wrong columns under the upcoming model reset.
        // `mFilters` itself is rebuilt below by
        // `RebuildFiltersFromConfiguration`.
        mSortFilterProxyModel->SetFilterRules({});
        mTableView->sortByColumn(-1, Qt::AscendingOrder);

        mModel->ConfigurationManager().Load(file.toStdString());
        // `Load` rewrites the configuration without emitting any
        // model signal; the reset re-initialises the header and
        // pulls the loaded `visible` flags via the wired
        // `modelReset -> ApplyColumnVisibility` connect.
        mModel->NotifyConfigurationReplaced();
        RebuildFiltersFromConfiguration();
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

QMenu *MainWindow::ViewMenu() const
{
    return ui->menuView;
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

void MainWindow::SaveConfigurationToPathForTest(const QString &path)
{
    DoSaveConfiguration(path);
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

void MainWindow::MirrorFiltersToConfiguration()
{
    // Snapshot the runtime map into the wire-format vector so
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
        if (a.row != b.row)
        {
            return a.row < b.row;
        }
        if (a.type != b.type)
        {
            return static_cast<int>(a.type) < static_cast<int>(b.type);
        }
        // Tie-break field-wise so duplicate filters still land in a
        // deterministic order.
        if (a.filterString != b.filterString)
        {
            return a.filterString < b.filterString;
        }
        if (a.matchType != b.matchType)
        {
            return a.matchType < b.matchType;
        }
        if (a.filterBegin != b.filterBegin)
        {
            return a.filterBegin < b.filterBegin;
        }
        if (a.filterEnd != b.filterEnd)
        {
            return a.filterEnd < b.filterEnd;
        }
        if (a.filterMinValue != b.filterMinValue)
        {
            return a.filterMinValue < b.filterMinValue;
        }
        if (a.filterMaxValue != b.filterMaxValue)
        {
            return a.filterMaxValue < b.filterMaxValue;
        }
        return a.filterValues < b.filterValues;
    });
    mModel->ConfigurationManager().SetFilters(std::move(snapshot));
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
        DoSaveConfiguration(file);
    }
    catch (std::exception &e)
    {
        QMessageBox::warning(this, "Error Saving Configuration", e.what());
    }
}

void MainWindow::DoSaveConfiguration(const QString &path)
{
    // The eager mirror at every mutation point already keeps
    // `mConfiguration.filters` current; the call here documents
    // intent and survives a future mutation point that forgets to
    // mirror. `Save` propagates `std::exception` on I/O failure;
    // both production callers wrap this in a `try / catch`.
    MirrorFiltersToConfiguration();
    mModel->ConfigurationManager().Save(path.toStdString());
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

bool MainWindow::DoLoadConfiguration(const QString &path)
{
    try
    {
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
    MirrorFiltersToConfiguration();
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
    const QString &filterId, const std::optional<loglib::LogConfiguration::LogFilter> &filter, bool openEditor
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
    MirrorFiltersToConfiguration();
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
    if (!deferSync)
    {
        MirrorFiltersToConfiguration();
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

void MainWindow::AddLogFilter(const QString &id, const loglib::LogConfiguration::LogFilter &filter, bool deferSync)
{
    mFilters[id.toStdString()] = filter;
    if (!deferSync)
    {
        MirrorFiltersToConfiguration();
        UpdateFilters();
    }

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
    case loglib::LogConfiguration::LogFilter::Type::Number:
    {
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
        title = QStringLiteral("[%1, %2]").arg(minStr, maxStr);
        break;
    }
    case loglib::LogConfiguration::LogFilter::Type::Boolean:
    {
        // Canonicalise to "true, false" order regardless of how
        // `filter.filterValues` is laid out (the submit slot always
        // writes "true" first, but a hand-edited config might not).
        const BooleanFilterSides sides = DecodeBooleanFilterSides(filter.filterValues);
        QStringList values;
        if (sides.includeTrue)
        {
            values.append(QStringLiteral("true"));
        }
        if (sides.includeFalse)
        {
            values.append(QStringLiteral("false"));
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
    // Capture only the filter id and resolve the live filter at
    // trigger time. Capturing the filter by value would freeze its
    // `row` at the column it had at menu-build time, so a column
    // reorder between build and click would mis-target Edit and the
    // type-match guard would silently drop the filter. Regression:
    // `TestEditFilterAfterColumnReorderUsesCurrentRow`.
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
    QMenu *menu = BuildHeaderContextMenu(logical, header);
    if (menu == nullptr)
    {
        return;
    }
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(header->mapToGlobal(pos));
}

QMenu *MainWindow::BuildHeaderContextMenu(int logicalColumn, QWidget *parent)
{
    const auto &columns = mModel->Configuration().columns;
    if (logicalColumn < 0 || static_cast<size_t>(logicalColumn) >= columns.size())
    {
        return nullptr;
    }
    auto *menu = new QMenu(parent != nullptr ? parent : mTableView);

    // Capture stable `keys` rather than the logical index: a column
    // move while the menu is open would otherwise leave the action
    // pointing at the wrong column. `FindColumnIndexByKeys`
    // re-resolves at trigger time.
    const std::vector<std::string> &thisKeys = columns[static_cast<size_t>(logicalColumn)].keys;
    const auto &thisColumn = columns[static_cast<size_t>(logicalColumn)];

    // O(N) bulk build; `ColumnMenuLabel` per column would be
    // O(N^2) on the `Show column` submenu.
    const std::vector<QString> labels = BuildAllColumnMenuLabels();

    // Only offer Hide for visible columns. Production right-clicks
    // always hit a visible section; the test seam may pass a hidden
    // index, where Hide would be a confusing no-op.
    if (thisColumn.visible)
    {
        const QString &hideLabel = labels[static_cast<size_t>(logicalColumn)];
        const QAction *hideAction = menu->addAction(tr("Hide \"%1\"").arg(hideLabel));
        connect(hideAction, &QAction::triggered, this, [this, keys = thisKeys]() {
            const int idx = FindColumnIndexByKeys(keys);
            if (idx >= 0)
            {
                SetColumnVisible(idx, false);
            }
        });
    }

    // Hidden columns populate the `Show column` submenu. The clicked
    // column is normally visible, so it would not appear here in
    // production -- tests can call this with a hidden index.
    std::vector<int> hiddenColumns;
    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (!columns[i].visible)
        {
            hiddenColumns.push_back(static_cast<int>(i));
        }
    }
    if (!hiddenColumns.empty())
    {
        if (!menu->isEmpty())
        {
            menu->addSeparator();
        }
        QMenu *showMenu = menu->addMenu(tr("Show column"));
        for (const int hiddenLogical : hiddenColumns)
        {
            const std::vector<std::string> &hiddenKeys = columns[static_cast<size_t>(hiddenLogical)].keys;
            const QString &hiddenLabel = labels[static_cast<size_t>(hiddenLogical)];
            const QAction *showAction = showMenu->addAction(hiddenLabel);
            connect(showAction, &QAction::triggered, this, [this, keys = hiddenKeys]() {
                const int idx = FindColumnIndexByKeys(keys);
                if (idx >= 0)
                {
                    SetColumnVisible(idx, true);
                }
            });
        }
    }
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
    const auto &columns = mModel->Configuration().columns;
    if (columns.empty())
    {
        // Disabled placeholder so an empty View menu is not silent.
        QAction *placeholder = viewMenu->addAction(tr("(no columns yet)"));
        placeholder->setEnabled(false);
        return;
    }
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
