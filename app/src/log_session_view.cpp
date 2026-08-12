#include "log_session_view.hpp"

#include "anchor_manager.hpp"
#include "log_filter_model.hpp"
#include "log_model.hpp"
#include "log_session.hpp"
#include "log_table_view.hpp"
#include "overview_rail_model.hpp"
#include "overview_rail_widget.hpp"
#include "row_order_proxy_model.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_processing.hpp>

#include <QAbstractItemModel>
#include <QAbstractItemView>
#include <QDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QIntValidator>
#include <QItemSelectionModel>
#include <QLabel>
#include <QLatin1Char>
#include <QLineEdit>
#include <QModelIndex>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QSizePolicy>
#include <QString>
#include <QVBoxLayout>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{
/// `QProgressBar::setRange` upper bound for percent-mode ticks.
/// Matches the shell's `PROGRESS_PERCENT_MAX` (100).
constexpr int PROGRESS_PERCENT_MAX = 100;
/// Progress strip layout metrics: horizontal margin (px),
/// vertical margin (px), inter-widget spacing (px). Pulled into
/// named constants so the review-touched code stays free of
/// magic-numbers clang-tidy warnings.
constexpr int PROGRESS_STRIP_MARGIN_H = 4;
constexpr int PROGRESS_STRIP_MARGIN_V = 2;
constexpr int PROGRESS_STRIP_SPACING = 6;
} // namespace

LogSessionView::LogSessionView(LogSession *session, ThemeControl *theme, QWidget *parent)
    : QWidget(parent), mSession(session)
{
    Q_ASSERT_X(mSession != nullptr, "LogSessionView", "session must not be null");
    Initialise(theme);
}

LogSessionView::LogSessionView(LogSession *session, QWidget *parent)
    : LogSessionView(session, nullptr, parent)
{
}

LogSessionView::~LogSessionView() = default;

void LogSessionView::Initialise(ThemeControl *theme)
{
    // Task 3.2/3.3: own the per-session widgets. Parenting is
    // load-bearing: the table + rail must die with this view
    // (which dies with the tab / session) so the shell's aliases
    // can be cleared under `~LogSessionView` before the model
    // quintet on `LogSession` reverse-teardown runs.
    //
    // Order matters at construction time:
    //
    //   1. `mTableView` first because the rail widget takes a
    //      non-owning `QAbstractItemView *` to resolve viewport
    //      geometry via its vertical scrollbar.
    //   2. Rail model + widget after: the model buckets the
    //      session's filter-proxy rows and the widget's viewport
    //      indicator reads the table's scrollbar. Both stay live
    //      while hidden so the toggle is instant on both edges
    //      (matches the pre-migration `MainWindow` behaviour).
    //
    // Layout choice: `mTableView` fills the view via a full-
    // margin `QVBoxLayout`. The rail widget is deliberately NOT
    // placed in a sibling layout slot because the pre-existing
    // shell uses `LogTableView::AttachOverviewRail(rail)` to
    // reparent the rail INTO the table view when the toggle
    // turns on (it then sits in the table's reserved right
    // viewport margin). Keeping the rail as a bare-parented
    // widget on `this` (hidden) preserves that dance, so no
    // shell-body rewire is needed for phase 3's structural
    // extraction.
    mTableView = new LogTableView(this);
    mTableView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    // A null session should be impossible in production (the ctor
    // asserts), but the model quintet may not be fully wired in
    // a unit-test fixture that constructs a bare `LogSession`
    // outside a `MainWindow`. Guard the accessors so
    // `LogSessionView` can still be instantiated cleanly in that
    // case; the shell wires the actual models by calling
    // `TableView()->setModel(...)` / `SetAnchorManager(...)` /
    // etc. after construction.
    QAbstractItemModel *filterProxy = nullptr;
    LogModel *sourceModel = nullptr;
    AnchorManager *anchors = nullptr;
    if (mSession != nullptr)
    {
        filterProxy = mSession->FilterProxy();
        sourceModel = mSession->Model();
        anchors = mSession->Anchors();
    }

    mOverviewRailModel = new OverviewRailModel(filterProxy, sourceModel, anchors, this);
    mOverviewRailWidget = new OverviewRailWidget(mOverviewRailModel, theme, mTableView, this);
    mOverviewRailWidget->hide();

    mLayout = new QVBoxLayout(this);
    mLayout->setContentsMargins(0, 0, 0, 0);
    mLayout->setSpacing(0);
    mLayout->addWidget(mTableView, 1);

    // Task 3.2 continuation (review finding #1): a standalone
    // `LogSessionView` must be independently functional. Before
    // this migration the shell called `setModel` / `SetAnchorManager`
    // / selection setup after constructing the view, so a bare
    // view had a null `selectionModel()` and any navigation body
    // that dereferenced it crashed. Bind the proxy model + anchor
    // manager + selection/sort/edit defaults + session-invariant
    // header setup here so `TableView()->selectionModel()` is
    // non-null the moment the view returns from its ctor.
    //
    // Header wiring that reaches into shell slots (context menu,
    // section-move handler) still lives on the shell; those
    // connections are added after construction and don't
    // participate in the view's independent-usability contract.
    if (filterProxy != nullptr)
    {
        mTableView->setModel(filterProxy);
    }
    if (anchors != nullptr)
    {
        mTableView->SetAnchorManager(anchors);
    }
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
    mTableView->setSortingEnabled(true);
    // Start unsorted so the initial view matches source order; the
    // shell restores the persisted sort column after configuration
    // load. `-1` on a sortable header means "no sort indicator".
    mTableView->sortByColumn(-1, Qt::SortOrder::AscendingOrder);

    if (auto *header = mTableView->horizontalHeader())
    {
        header->setSectionResizeMode(QHeaderView::Interactive);
        header->resizeSections(QHeaderView::Stretch);
        header->setStretchLastSection(true);
        header->setHighlightSections(false);
        header->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        header->setSectionsMovable(true);
        // Cycle Asc -> Desc -> none. The "no sort" state restores
        // arrival order. Requires Qt 6.1+.
        header->setSortIndicatorClearable(true);
    }
}

void LogSessionView::SelectSourceRow(int sourceRow)
{
    // Migrated from `MainWindow::SelectSourceRow` (task 3.4). The
    // shell keeps the `statusBar()->showMessage(...)` call on its
    // side via the `rowNotVisible()` signal below; the view has
    // no business talking to `QMainWindow::statusBar()`.
    if (mSession == nullptr || mTableView == nullptr)
    {
        return;
    }
    auto *model = mSession->Model();
    auto *rowOrder = mSession->RowOrderProxy();
    auto *filterProxy = mSession->FilterProxy();
    if (model == nullptr || rowOrder == nullptr || filterProxy == nullptr)
    {
        return;
    }
    if (sourceRow < 0 || sourceRow >= model->rowCount())
    {
        // Caller's row is stale (evicted or session swap). Surface
        // it instead of silently no-oping. Shell wires this to a
        // status-bar message.
        emit rowNotVisible();
        return;
    }
    const QModelIndex sourceIdx = model->index(sourceRow, 0);
    const QModelIndex midIdx = rowOrder->mapFromSource(sourceIdx);
    if (!midIdx.isValid())
    {
        emit rowNotVisible();
        return;
    }
    const QModelIndex proxyIdx = filterProxy->mapFromSource(midIdx);
    if (!proxyIdx.isValid())
    {
        emit rowNotVisible();
        return;
    }

    mTableView->clearSelection();
    // Centre so the user sees context around the anchor.
    mTableView->scrollTo(proxyIdx, QAbstractItemView::PositionAtCenter);
    mTableView->selectionModel()->select(proxyIdx, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    mTableView->selectionModel()->setCurrentIndex(proxyIdx, QItemSelectionModel::NoUpdate);
}

void LogSessionView::ScrollToProxyRow(int proxyRow, bool replaceSelection)
{
    // Migrated from `MainWindow::ScrollToProxyRow` (task 3.4). The
    // shell keeps `ui->actionFollowTail` on its side; the view
    // fires `followTailDisengageRequested()` before scrolling so
    // the shell can uncheck the action first.
    if (mSession == nullptr || mTableView == nullptr)
    {
        return;
    }
    auto *filterProxy = mSession->FilterProxy();
    if (filterProxy == nullptr)
    {
        return;
    }
    if (proxyRow < 0 || proxyRow >= filterProxy->rowCount())
    {
        // Silently no-op on a transient out-of-range so a drag
        // scrub stays smooth during insert / filter races.
        return;
    }
    const QModelIndex proxyIdx = filterProxy->index(proxyRow, 0);
    if (!proxyIdx.isValid())
    {
        return;
    }
    // Rail navigation is intentional browsing; ask the shell to
    // disengage Follow newest so a live-tail batch can't yank the
    // viewport back to the tail. `scrollTo` is programmatic and
    // wouldn't fire `userScrolledAwayFromTail` on its own.
    emit followTailDisengageRequested();
    mTableView->scrollTo(proxyIdx, QAbstractItemView::PositionAtCenter);
    if (replaceSelection)
    {
        // Fresh click: same "commit to row" semantics as
        // `SelectSourceRow`.
        mTableView->clearSelection();
        mTableView->selectionModel()->select(proxyIdx, QItemSelectionModel::Select | QItemSelectionModel::Rows);
        mTableView->selectionModel()->setCurrentIndex(proxyIdx, QItemSelectionModel::NoUpdate);
    }
    // Drag scrub: scroll only, leave selection + current index
    // untouched so exploration doesn't clobber the last-clicked row.
}

void LogSessionView::ApplyColumnVisibility()
{
    // Migrated from `MainWindow::ApplyColumnVisibility` (task 3.5).
    // Shell-scoped follow-ups (`OnFindCacheInvalidated`,
    // `SyncColumnFilterIndicators`) stay on the shell's wrapper
    // because they reach into the find dock / filter menus which
    // belong to shell chrome, not the view.
    if (mSession == nullptr || mTableView == nullptr)
    {
        return;
    }
    auto *model = mSession->Model();
    if (model == nullptr)
    {
        return;
    }
    QHeaderView *header = mTableView->horizontalHeader();
    if (header == nullptr)
    {
        return;
    }
    const auto &columns = model->Configuration().columns;
    const std::size_t end = std::min(columns.size(), static_cast<std::size_t>(std::max(0, header->count())));
    for (std::size_t i = 0; i < end; ++i)
    {
        header->setSectionHidden(static_cast<int>(i), !columns[i].visible);
    }
}

void LogSessionView::ApplyLevelCellDelegate(QAbstractItemDelegate *delegate)
{
    // Migrated from `MainWindow::ApplyLevelCellDelegate` (task 3.5).
    // Delegate ownership stays on the shell (per-window resource);
    // this method just manages install/detach against the table.
    if (mSession == nullptr || mTableView == nullptr)
    {
        return;
    }
    auto *model = mSession->Model();
    if (delegate == nullptr || model == nullptr)
    {
        // Matches pre-migration `MainWindow::ApplyLevelCellDelegate`
        // (post-review-3 semantic-alignment): the no-theme test
        // fixture path passes `delegate == nullptr` and the
        // original returned early WITHOUT touching the table.
        // The delegate never transitions non-null -> null in
        // production (theme presence is a ctor invariant), so
        // there is nothing to detach on this path -- the earlier
        // migration added a speculative detach that was dead
        // code with a different semantic. Leave the return
        // early to keep behaviour byte-for-byte with `main`.
        return;
    }

    // Detach in text mode (don't just rely on the delegate's
    // self-gate) so text-mode paints skip the proxy-chain walk
    // inside the delegate.
    const bool iconMode = model->IsLevelIconModeActive();
    const int newColumn = iconMode ? model->FirstLevelColumnIndex() : -1;

    // Detach from the previous column when the level column has
    // moved -- otherwise the delegate would suppress text on the
    // old column after a reload.
    if (mInstalledLevelDelegateColumn >= 0 && mInstalledLevelDelegateColumn != newColumn)
    {
        // `nullptr` reverts to the default delegate; Qt keeps
        // ownership of the delegate via the shell's object tree.
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

    mTableView->setItemDelegateForColumn(newColumn, delegate);
    mInstalledLevelDelegateColumn = newColumn;
}

namespace
{

/// True if @p fmt contains a `date::parse` zone specifier (`%z`,
/// `%Z`, `%Ez`, `%Oz`); such a parse yields UTC and must NOT be
/// TZ-shifted again. `%%` is a literal percent and does not
/// register. On a non-match the scan only advances one byte so a
/// malformed prefix like `"%E%z"` still detects the inner `%z`
/// (false negatives here would double-shift downstream, worse
/// than false-positive-recognising a bad format).
[[nodiscard]] bool FormatHasZoneSpecifier(std::string_view fmt) noexcept
{
    for (std::size_t i = 0; i < fmt.size(); ++i)
    {
        if (fmt[i] != '%')
        {
            continue;
        }
        const std::size_t next = i + 1;
        if (next >= fmt.size())
        {
            break;
        }
        if (fmt[next] == '%')
        {
            // `%%` is a literal percent; skip both bytes.
            ++i;
            continue;
        }
        std::size_t specifier = next;
        if ((fmt[specifier] == 'E' || fmt[specifier] == 'O') && specifier + 1 < fmt.size())
        {
            ++specifier;
        }
        if (fmt[specifier] == 'z' || fmt[specifier] == 'Z')
        {
            return true;
        }
        // Only the outer `++i` advances; jumping past `specifier`
        // would miss a `%z` following a malformed `%E`.
    }
    return false;
}

} // namespace

std::optional<LogSessionView::GotoTimestampParse> LogSessionView::ParseGotoTimestampInput(
    const QString &input, const std::vector<std::string> &columnParseFormats, std::chrono::system_clock::time_point now
)
{
    const QString trimmed = input.trimmed();
    if (trimmed.isEmpty())
    {
        return std::nullopt;
    }

    // Relative shortcut `[+-]?N[hm]` (case-insensitive,
    // whitespace-tolerant). All sign variants mean "N units before
    // @p now" -- "the future" is meaningless in a log viewer, and
    // lnav / less resolve the same way.
    static const QRegularExpression RELATIVE_SHORTCUT_RE(
        QStringLiteral(R"(^[+-]?\s*(\d+)\s*([hm])\s*$)"), QRegularExpression::CaseInsensitiveOption
    );
    const auto relMatch = RELATIVE_SHORTCUT_RE.match(trimmed);
    if (relMatch.hasMatch())
    {
        bool ok = false;
        const qulonglong n = relMatch.captured(1).toULongLong(&ok);
        if (!ok)
        {
            return std::nullopt;
        }
        const QChar unit = relMatch.captured(2).at(0).toLower();
        // Reject values that would overflow `int64_t` micros --
        // wrapping silently would jump the user forward, not back.
        constexpr int64_t MICROS_PER_HOUR = 3'600LL * 1'000'000LL;
        constexpr int64_t MICROS_PER_MINUTE = 60LL * 1'000'000LL;
        const int64_t microsPerUnit = (unit == QLatin1Char('h')) ? MICROS_PER_HOUR : MICROS_PER_MINUTE;
        const auto maxN = static_cast<qulonglong>(std::numeric_limits<int64_t>::max() / microsPerUnit);
        if (n > maxN)
        {
            return std::nullopt;
        }
        const int64_t offsetMicros = static_cast<int64_t>(n) * microsPerUnit;
        const auto nowMicros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        return GotoTimestampParse{.micros = nowMicros - offsetMicros, .isNaive = false};
    }

    // Absolute path: try the column's own `parseFormats` first,
    // then two ISO fallbacks so columns with an empty format list
    // (auto-detected `Type::Time`) still accept typical inputs.
    const std::string stdInput = trimmed.toStdString();
    std::vector<std::string> candidates;
    candidates.reserve(columnParseFormats.size() + 2);
    for (const auto &fmt : columnParseFormats)
    {
        candidates.push_back(fmt);
    }
    for (const auto *fallback : {"%FT%T", "%F %T"})
    {
        if (std::find(candidates.begin(), candidates.end(), std::string{fallback}) == candidates.end())
        {
            candidates.emplace_back(fallback);
        }
    }

    loglib::TimestampParseScratch scratch;
    for (const auto &fmt : candidates)
    {
        loglib::TimeStamp parsed{};
        if (loglib::TryParseTimestamp(stdInput, fmt, loglib::ClassifyTimestampFormat(fmt), scratch, parsed))
        {
            return GotoTimestampParse{
                .micros = parsed.time_since_epoch().count(), .isNaive = !FormatHasZoneSpecifier(fmt)
            };
        }
    }
    return std::nullopt;
}

void LogSessionView::PromptGotoLine()
{
    // Migrated from `MainWindow::GotoLine` (task 3.6). Modal +
    // sticky-input storage live on the view; status-bar feedback
    // fans out via `statusMessageRequested()`.
    if (mSession == nullptr)
    {
        return;
    }
    auto *model = mSession->Model();
    if (model == nullptr || model->rowCount() == 0)
    {
        emit statusMessageRequested(tr("No log loaded."));
        return;
    }

    const int rowCount = model->rowCount();

    // A `QInputDialog` (rather than the static `getInt` helper) so
    // we can attach a `QIntValidator` and lean on Qt's accept-
    // blocking instead of re-validating after `exec`. The label
    // uses `QString::number` (not `QLocale`) to match the
    // validator, which has no group separator -- otherwise `en_US`
    // shows commas in a range the user cannot actually type.
    QInputDialog dialog(this);
    dialog.setWindowTitle(tr("Go to Line"));
    // Reminder: with newest-first display active, "line 1" is
    // still the earliest row (bottom of the list), not the top.
    dialog.setLabelText(tr("Line number (1 = earliest row; 1 - %1):").arg(QString::number(rowCount)));
    dialog.setInputMode(QInputDialog::TextInput);
    dialog.setTextEchoMode(QLineEdit::Normal);
    if (!mLastGotoLineInput.isEmpty())
    {
        dialog.setTextValue(mLastGotoLineInput);
    }
    if (auto *editor = dialog.findChild<QLineEdit *>())
    {
        editor->setValidator(new QIntValidator(1, rowCount, &dialog));
        editor->setPlaceholderText(tr("e.g. 12345"));
        editor->selectAll();
    }
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    const QString rawInput = dialog.textValue();
    mLastGotoLineInput = rawInput;
    ExecuteGotoLine(rawInput);
}

void LogSessionView::ExecuteGotoLine(const QString &input)
{
    // Read `rowCount` live (not captured before the modal) so a
    // shrink while the dialog was open -- FIFO eviction or session
    // swap -- is reflected in both the range check and the hint.
    if (mSession == nullptr)
    {
        emit statusMessageRequested(tr("No log loaded."));
        return;
    }
    auto *model = mSession->Model();
    auto *rowOrder = mSession->RowOrderProxy();
    auto *filterProxy = mSession->FilterProxy();
    if (model == nullptr || rowOrder == nullptr || filterProxy == nullptr)
    {
        emit statusMessageRequested(tr("No log loaded."));
        return;
    }
    const int currentRowCount = model->rowCount();
    if (currentRowCount == 0)
    {
        emit statusMessageRequested(tr("No log loaded."));
        return;
    }

    bool ok = false;
    const int oneBased = input.toInt(&ok);
    if (!ok || oneBased < 1 || oneBased > currentRowCount)
    {
        emit statusMessageRequested(
            tr("Line %1 is out of range (1 - %2).").arg(input, QString::number(currentRowCount))
        );
        return;
    }

    // Print the caller-specific "filtered out" hint before
    // deferring to `SelectSourceRow`, which would otherwise show
    // the generic "Row is not currently visible" fallback.
    const int sourceRow = oneBased - 1;
    const QModelIndex sourceIdx = model->index(sourceRow, 0);
    const QModelIndex midIdx = rowOrder->mapFromSource(sourceIdx);
    const bool visible = midIdx.isValid() && filterProxy->mapFromSource(midIdx).isValid();
    if (!visible)
    {
        emit statusMessageRequested(tr("Line %1 is currently filtered out.").arg(QString::number(oneBased)));
        return;
    }

    SelectSourceRow(sourceRow);
}

void LogSessionView::PromptGotoTimestamp()
{
    // Migrated from `MainWindow::GotoTimestamp` (task 3.6). See
    // `PromptGotoLine` for the modal-plus-status-signal pattern.
    if (mSession == nullptr)
    {
        return;
    }
    auto *model = mSession->Model();
    if (model == nullptr || model->rowCount() == 0)
    {
        emit statusMessageRequested(tr("No log loaded."));
        return;
    }

    const int timeCol = loglib::FirstTimeColumnIndex(model->Configuration());
    if (timeCol < 0)
    {
        emit statusMessageRequested(tr("This log has no timestamp column."));
        return;
    }

    QInputDialog dialog(this);
    dialog.setWindowTitle(tr("Go to Timestamp"));
    // "local time" makes clear that a bare `YYYY-MM-DD HH:MM:SS`
    // is interpreted in the display TZ, matching the table.
    // Raw `Z`-suffixed values from the file hit the zoned parser
    // and are not shifted.
    dialog.setLabelText(tr("Timestamp (local time; ISO 8601 or -Nh / -Nm):"));
    dialog.setInputMode(QInputDialog::TextInput);
    dialog.setTextEchoMode(QLineEdit::Normal);
    dialog.setTextValue(mLastGotoTimestampInput);
    if (auto *editor = dialog.findChild<QLineEdit *>())
    {
        editor->setPlaceholderText(tr("e.g. 2024-04-28 12:34:56 (local) or -1h / -30m"));
        editor->selectAll();
    }
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }

    ExecuteGotoTimestamp(dialog.textValue(), std::chrono::system_clock::now());
}

void LogSessionView::ExecuteGotoTimestamp(const QString &input, std::chrono::system_clock::time_point now)
{
    // Read model + config live -- mirrors `ExecuteGotoLine`.
    if (mSession == nullptr)
    {
        emit statusMessageRequested(tr("No log loaded."));
        return;
    }
    auto *model = mSession->Model();
    if (model == nullptr || model->rowCount() == 0)
    {
        emit statusMessageRequested(tr("No log loaded."));
        return;
    }
    const auto &config = model->Configuration();
    const int timeCol = loglib::FirstTimeColumnIndex(config);
    if (timeCol < 0)
    {
        emit statusMessageRequested(tr("This log has no timestamp column."));
        return;
    }

    // Update the sticky input before the parse so a garbage value
    // the user wants to correct is retained on the next open.
    mLastGotoTimestampInput = input;

    const std::optional<GotoTimestampParse> parsed =
        ParseGotoTimestampInput(input, config.columns[static_cast<std::size_t>(timeCol)].parseFormats, now);
    if (!parsed.has_value())
    {
        emit statusMessageRequested(tr("Could not parse timestamp."));
        return;
    }
    // Stored cell timestamps line up with the TZ-shifted display:
    // zoned columns hold true UTC micros, naive columns hold the
    // wall-clock digits which are then TZ-shifted for rendering.
    // Naive user input represents a display-zone wall-clock
    // instant, so it needs the same local -> UTC shift to match.
    // `LocalMicrosecondsSinceEpochToUtc` handles DST edge cases.
    const int64_t targetMicros =
        parsed->isNaive ? loglib::LocalMicrosecondsSinceEpochToUtc(parsed->micros) : parsed->micros;

    const int sourceRow = mSession->FindFirstRowAtOrAfterTimestamp(timeCol, targetMicros);
    if (sourceRow < 0)
    {
        emit statusMessageRequested(tr("No visible row at or after that time."));
        return;
    }
    SelectSourceRow(sourceRow);
}

void LogSessionView::JumpToNewestRow()
{
    // Migrated from `MainWindow::JumpToNewestRow` (task 3.4). No
    // shell-scoped state referenced -- uses only model + proxy
    // chain + table view.
    if (mSession == nullptr || mTableView == nullptr)
    {
        return;
    }
    auto *model = mSession->Model();
    auto *rowOrder = mSession->RowOrderProxy();
    auto *filterProxy = mSession->FilterProxy();
    if (model == nullptr || rowOrder == nullptr || filterProxy == nullptr)
    {
        return;
    }
    const int sourceRowCount = model->rowCount();
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
    const QModelIndex sourceIndex = model->index(sourceRowCount - 1, 0);
    const QModelIndex midIndex = rowOrder->mapFromSource(sourceIndex);
    QModelIndex proxyIndex = filterProxy->mapFromSource(midIndex);

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
            const QModelIndex candidateSource = model->index(sourceRowCount - 1 - offset, 0);
            const QModelIndex candidateMid = rowOrder->mapFromSource(candidateSource);
            const QModelIndex candidateProxy = filterProxy->mapFromSource(candidateMid);
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
        const int proxyRowCount = filterProxy->rowCount();
        if (proxyRowCount <= 0)
        {
            // Nothing visible to scroll to. Clear the pending
            // announcement -- it can't refer to any row.
            mTableView->AcknowledgePendingNewRows();
            return;
        }
        const int targetRow = tailIsTop ? 0 : (proxyRowCount - 1);
        proxyIndex = filterProxy->index(targetRow, 0);
        if (!proxyIndex.isValid())
        {
            return;
        }
    }

    const auto position = tailIsTop ? QAbstractItemView::PositionAtTop : QAbstractItemView::PositionAtBottom;
    mTableView->scrollTo(proxyIndex, position);
}

void LogSessionView::JumpToAnchor(bool forward)
{
    // Migrated from `MainWindow::JumpToAnchor` (task 3.4). Uses
    // only session-owned models + view; status feedback fans out
    // via `statusMessageRequested()`.
    if (mSession == nullptr || mTableView == nullptr)
    {
        return;
    }
    auto *anchors = mSession->Anchors();
    auto *model = mSession->Model();
    auto *rowOrder = mSession->RowOrderProxy();
    auto *filterProxy = mSession->FilterProxy();
    if (anchors == nullptr || model == nullptr || rowOrder == nullptr || filterProxy == nullptr)
    {
        return;
    }
    if (anchors->Empty())
    {
        emit statusMessageRequested(tr("No anchors set."));
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
        emit statusMessageRequested(tr("No anchored rows are currently visible."));
        return;
    }

    // Enumerate anchors rather than walking every proxy row: the
    // anchor count is bounded by user clicks while the proxy can
    // be tens of thousands of rows deep on a streaming session.
    const auto anchorEntries = anchors->Entries();
    std::vector<int> anchoredProxyRows;
    anchoredProxyRows.reserve(anchorEntries.size());
    for (const auto &entry : anchorEntries)
    {
        const AnchorManager::Key key{.locator = entry.locator, .lineId = entry.lineId};
        const int sourceRow = model->SourceRowForAnchorKey(key);
        if (sourceRow < 0)
        {
            // Anchor outlived its row.
            continue;
        }
        const QModelIndex sourceIdx = model->index(sourceRow, 0);
        const QModelIndex midIdx = rowOrder->mapFromSource(sourceIdx);
        if (!midIdx.isValid())
        {
            continue;
        }
        const QModelIndex proxyIdx = filterProxy->mapFromSource(midIdx);
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
        emit statusMessageRequested(tr("No anchored rows are currently visible."));
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

void LogSessionView::EnsureProgressStrip()
{
    if (mProgressStrip != nullptr)
    {
        return;
    }
    // Lazy construction so an empty session that never runs a
    // background operation stays free of the widget overhead.
    // Sits below the table in the outer `QVBoxLayout` (added at
    // ctor time; the strip becomes the second layout entry).
    mProgressStrip = new QWidget(this);
    mProgressStrip->setObjectName(QStringLiteral("sessionProgressStrip"));
    auto *stripLayout = new QHBoxLayout(mProgressStrip);
    stripLayout->setContentsMargins(
        PROGRESS_STRIP_MARGIN_H, PROGRESS_STRIP_MARGIN_V, PROGRESS_STRIP_MARGIN_H, PROGRESS_STRIP_MARGIN_V
    );
    stripLayout->setSpacing(PROGRESS_STRIP_SPACING);

    mProgressLabel = new QLabel(mProgressStrip);
    mProgressLabel->setObjectName(QStringLiteral("sessionProgressLabel"));
    mProgressBar = new QProgressBar(mProgressStrip);
    mProgressBar->setObjectName(QStringLiteral("sessionProgressBar"));
    mProgressBar->setRange(0, PROGRESS_PERCENT_MAX);
    mProgressBar->setValue(0);
    mProgressBar->setTextVisible(true);
    mProgressCancelButton = new QPushButton(tr("Cancel"), mProgressStrip);
    mProgressCancelButton->setObjectName(QStringLiteral("sessionProgressCancelButton"));

    stripLayout->addWidget(mProgressLabel, 0);
    stripLayout->addWidget(mProgressBar, 1);
    stripLayout->addWidget(mProgressCancelButton, 0);

    mProgressStrip->hide();
    connect(mProgressCancelButton, &QPushButton::clicked, this, [this]() { emit progressCancelRequested(); });

    // Append below the table (which is already at layout index 0
    // from the ctor). The rail widget is parented on `this` but
    // NOT in the layout (see ctor comment about `AttachOverviewRail`),
    // so this addition doesn't perturb the rail's attach dance.
    if (mLayout != nullptr)
    {
        mLayout->addWidget(mProgressStrip, 0);
    }
}

void LogSessionView::ShowOperationProgress(const QString &label, int percent)
{
    EnsureProgressStrip();
    mProgressLabel->setText(label);
    if (percent < 0)
    {
        // Busy indicator: `min == max == 0` renders as marching
        // ants in the platform style.
        mProgressBar->setRange(0, 0);
    }
    else
    {
        if (mProgressBar->minimum() != 0 || mProgressBar->maximum() != PROGRESS_PERCENT_MAX)
        {
            mProgressBar->setRange(0, PROGRESS_PERCENT_MAX);
        }
        mProgressBar->setValue(std::clamp(percent, 0, PROGRESS_PERCENT_MAX));
    }
    // `isHidden()` (not `isVisible()`) so the request-to-show is
    // honoured even when the outer view hasn't been placed into a
    // shown ancestor yet (typical test-fixture path).
    if (mProgressStrip->isHidden())
    {
        mProgressStrip->show();
    }
}

void LogSessionView::UpdateOperationProgress(const QString &label, int percent)
{
    if (mProgressStrip == nullptr || mProgressStrip->isHidden())
    {
        // Update on a hidden strip is a caller error (they should
        // have used `ShowOperationProgress` first). Silently no-op
        // rather than construct-then-hide, which would leak
        // widgets on rapid tick-without-show paths. `isHidden()`
        // (not `isVisible()`) so a strip whose ancestor hasn't
        // been shown yet still counts as "user asked us to show".
        return;
    }
    mProgressLabel->setText(label);
    if (percent < 0)
    {
        mProgressBar->setRange(0, 0);
    }
    else
    {
        if (mProgressBar->minimum() != 0 || mProgressBar->maximum() != PROGRESS_PERCENT_MAX)
        {
            mProgressBar->setRange(0, PROGRESS_PERCENT_MAX);
        }
        mProgressBar->setValue(std::clamp(percent, 0, PROGRESS_PERCENT_MAX));
    }
}

void LogSessionView::HideOperationProgress()
{
    if (mProgressStrip == nullptr || mProgressStrip->isHidden())
    {
        return;
    }
    mProgressStrip->hide();
    // Reset so a future show starts from a known state rather than
    // flashing the last percentage of the previous operation.
    if (mProgressBar != nullptr)
    {
        mProgressBar->setRange(0, PROGRESS_PERCENT_MAX);
        mProgressBar->setValue(0);
    }
    if (mProgressLabel != nullptr)
    {
        mProgressLabel->clear();
    }
}

bool LogSessionView::IsOperationProgressVisible() const noexcept
{
    // `isHidden()` inverts the "show requested" flag; unlike
    // `isVisible()` it doesn't consult ancestor visibility, which
    // matches what a caller means when they ask "did we show the
    // strip to the user (given an on-screen tab)?".
    return mProgressStrip != nullptr && !mProgressStrip->isHidden();
}
