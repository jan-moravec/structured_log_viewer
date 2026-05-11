#include "log_filter_model.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/log_compare.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_table.hpp>

#include <QAbstractItemModel>
#include <QAbstractProxyModel>
#include <QDebug>
#include <QModelIndex>
#include <QObject>
#include <QRegularExpression>
#include <QString>
#include <QVariant>

#include <algorithm>
#include <cstddef>
#include <mutex>
#include <utility>

LogFilterModel::LogFilterModel(QObject *parent)
    : QSortFilterProxyModel{parent}
{
}

void LogFilterModel::SetLogModel(LogModel *logModel)
{
    mLogModel = logModel;
    mEnumRanks.clear();
}

void LogFilterModel::setSourceModel(QAbstractItemModel *sourceModel)
{
    // Defensively wipe filter state before the base class re-filter so
    // a synchronous walk under the new chain cannot evaluate
    // predicates baked against the old `LogTable` (their
    // `EnumValueId` bitsets and column indices alias the previous
    // dictionary). The new chain's `LogModel` must be re-wired via
    // `SetLogModel`; documented contract on that method's header
    // comment. The rank cache is cleared on the same beat; it's keyed
    // by `KeyId` (stable across column reorders), so there's no
    // separate `columnsMoved` hook to maintain here.
    mFilterRules.clear();
    mLogModel = nullptr;
    mEnumRanks.clear();

    QSortFilterProxyModel::setSourceModel(sourceModel);
    RefreshSourceProxyCache();
}

void LogFilterModel::RefreshSourceProxyCache()
{
    mImmediateProxy = qobject_cast<QAbstractProxyModel *>(sourceModel());
}

void LogFilterModel::SetFilterRules(std::vector<loglib::RowPredicate> &&filterRules)
{
    // No-op guard: when both the current and the incoming rule lists
    // are empty, the proxy's filter decision is already "accept every
    // row" and rerunning `endFilterChange` / `invalidateFilter` would
    // just force a redundant row-map rebuild. The guard only triggers
    // when nothing actually changes; once non-empty rules have been
    // applied, transitioning back to empty goes through the
    // invalidation path below.
    if (mFilterRules.empty() && filterRules.empty())
    {
        return;
    }
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    beginFilterChange();
    mFilterRules = std::move(filterRules);
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
#else
    mFilterRules = std::move(filterRules);
    invalidateFilter();
#endif
}

void LogFilterModel::InvalidateEnumRanks()
{
    mEnumRanks.clear();
}

bool LogFilterModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (mFilterRules.empty())
    {
        return true;
    }
    // Predicates can't run without the table. We get here only when
    // `SetFilterRules` was called without first wiring a `LogModel` --
    // always a bug; debug builds trip the assertion below. Release
    // builds emit one diagnostic and reject every row so the misuse is
    // loud (the filter visibly hides everything) instead of silent
    // (every row accepted as if no rule were installed).
    Q_ASSERT_X(
        mLogModel != nullptr,
        "LogFilterModel::filterAcceptsRow",
        "filter rules set without a LogModel; call SetLogModel before SetFilterRules"
    );
    if (mLogModel == nullptr)
    {
        static std::once_flag warnedNoLogModelFlag;
        std::call_once(warnedNoLogModelFlag, [] {
            qWarning() << "LogFilterModel: filter rules present but mLogModel is null; rejecting every row";
        });
        return false;
    }
    const int logRow = MapToLogModelRow(sourceRow, sourceParent);
    if (logRow < 0)
    {
        return false;
    }
    const auto row = static_cast<size_t>(logRow);
    const loglib::LogTable &table = mLogModel->Table();
    return std::ranges::all_of(mFilterRules, [&table, row](const auto &rule) {
        return loglib::MatchesRow(rule, table, row);
    });
}

bool LogFilterModel::lessThan(const QModelIndex &sourceLeft, const QModelIndex &sourceRight) const
{
    // Production sort paths always have a LogModel attached (set by
    // `MainWindow::SetLogModel` before any sort is issued). The
    // fallback to the base class lets isolated unit tests build a
    // bare `LogFilterModel` and still sort via the default
    // `QVariant`-mediated path; it also covers the transient state
    // between `setSourceModel` (which clears `mLogModel`) and the
    // follow-up `SetLogModel` re-wire, when a previously-installed
    // sort column triggers a re-sort under Qt's `dynamicSortFilter`.
    // The fallback produces correct chronological / lexicographic
    // order when `SortRole` returns a `QVariant`-comparable scalar,
    // and degrades to string compare otherwise. In production this
    // path is unreachable once the wiring settles, so emit a
    // one-shot warning to flag a stuck misconfiguration without
    // tripping the test harness.
    if (mLogModel == nullptr)
    {
        static std::once_flag warnedNoLogModelFlag;
        std::call_once(warnedNoLogModelFlag, [] {
            qWarning() << "LogFilterModel::lessThan: called without a LogModel; falling back to QVariant compare";
        });
        return QSortFilterProxyModel::lessThan(sourceLeft, sourceRight);
    }
    const int leftRow = MapModelIndexToLogModelRow(sourceLeft);
    const int rightRow = MapModelIndexToLogModelRow(sourceRight);
    if (leftRow < 0 || rightRow < 0)
    {
        return QSortFilterProxyModel::lessThan(sourceLeft, sourceRight);
    }
    const int columnIndex = sourceLeft.column();
    const auto &columns = mLogModel->Configuration().columns;
    if (columnIndex < 0 || static_cast<size_t>(columnIndex) >= columns.size())
    {
        return QSortFilterProxyModel::lessThan(sourceLeft, sourceRight);
    }
    const loglib::EnumDictRank *rank = nullptr;
    if (columns[columnIndex].type == loglib::LogConfiguration::Type::Enumeration)
    {
        rank = EnumRankFor(columnIndex);
    }
    const int cmp = loglib::CompareRows(
        mLogModel->Table(),
        static_cast<size_t>(leftRow),
        static_cast<size_t>(rightRow),
        static_cast<size_t>(columnIndex),
        rank
    );
    return cmp < 0;
}

int LogFilterModel::MapToLogModelRow(int sourceRow, const QModelIndex &sourceParent) const
{
    const QAbstractItemModel *src = sourceModel();
    if (src == nullptr)
    {
        return -1;
    }
    return MapModelIndexToLogModelRow(src->index(sourceRow, 0, sourceParent));
}

int LogFilterModel::MapModelIndexToLogModelRow(QModelIndex idx) const
{
    if (!idx.isValid())
    {
        return -1;
    }
    // Fast path: cached immediate proxy is the single hop between this
    // filter and the underlying `LogModel`. Avoids the per-call
    // `qobject_cast` that QSortFilterProxyModel::sort would otherwise
    // pay O(N log N) times.
    if (mImmediateProxy != nullptr && idx.model() == mImmediateProxy)
    {
        const QModelIndex mapped = mImmediateProxy->mapToSource(idx);
        if (mapped.model() == mLogModel)
        {
            return mapped.row();
        }
        idx = mapped;
    }
    // Generic fall-through: walk proxy layers down to the underlying
    // `LogModel`. Covers deeper chains and `setSourceModel` calls that
    // happened before the cache caught up.
    while (const auto *proxy = qobject_cast<const QAbstractProxyModel *>(idx.model()))
    {
        const QModelIndex mapped = proxy->mapToSource(idx);
        if (!mapped.isValid())
        {
            return -1;
        }
        idx = mapped;
    }
    if (idx.model() != mLogModel)
    {
        return -1;
    }
    return idx.row();
}

loglib::KeyId LogFilterModel::EnumKeyForColumn(int columnIndex) const
{
    if (mLogModel == nullptr || columnIndex < 0)
    {
        return loglib::INVALID_KEY_ID;
    }
    return mLogModel->Table().ResolveEnumColumn(static_cast<size_t>(columnIndex)).canonicalKey;
}

const loglib::EnumDictRank *LogFilterModel::EnumRankFor(int columnIndex) const
{
    if (mLogModel == nullptr || columnIndex < 0)
    {
        return nullptr;
    }
    const auto lookup = mLogModel->Table().ResolveEnumColumn(static_cast<size_t>(columnIndex));
    if (lookup.canonicalKey == loglib::INVALID_KEY_ID || lookup.dictionary == nullptr)
    {
        return nullptr;
    }
    // Rebuild when the cached rank is missing, smaller than the live
    // dictionary (growth since the last access), or attached to a
    // *different* `EnumDictionary` instance than the live one. The
    // pointer check covers demote -> re-promote that re-creates the
    // registry entry at the same `Size()` -- without it the cache
    // would hand out a stale rank whose internal `EnumValueId` indices
    // mean different bytes.
    if (auto it = mEnumRanks.find(lookup.canonicalKey); it != mEnumRanks.end() &&
                                                        it->second.source == lookup.dictionary &&
                                                        it->second.rank.DictSize() >= lookup.dictionary->Size())
    {
        return &it->second.rank;
    }
    // `unordered_map::insert_or_assign` keeps references to other
    // stored values stable; the returned reference here is therefore
    // safe to alias as a raw pointer for the call's lifetime.
    const auto [it, inserted] = mEnumRanks.insert_or_assign(
        lookup.canonicalKey,
        EnumRankEntry{.rank = loglib::EnumDictRank{*lookup.dictionary}, .source = lookup.dictionary}
    );
    return &it->second.rank;
}

QList<QModelIndex> LogFilterModel::MatchRow(
    const QModelIndex &start,
    int role,
    const QVariant &value,
    int hits,
    Qt::MatchFlags flags,
    bool forward,
    int skipFirstN
) const
{
    QList<QModelIndex> result;
    const int rowCount = this->rowCount(start.parent());
    const int columnCount = this->columnCount(start.parent());

    const bool wrap = flags.testFlag(Qt::MatchWrap);
    const int startRow = start.row();
    const int startColumn = start.column();

    // Fast path: when matching `DisplayRole` against the production
    // `LogModel`, materialise each cell via
    // `LogTable::GetFormattedValue` + `ConvertToSingleLineCompactQString`
    // and skip the `data(role).toString()` round-trip through
    // `LogModel::data` / `QVariant<QString>`. Other roles fall back to
    // the QVariant path (kept for defensiveness; no production caller
    // uses a non-Display role here today).
    const bool useFastPath = role == Qt::DisplayRole && mLogModel != nullptr;
    const QString needle = useFastPath ? value.toString() : QString{};

    // `logRowCached` is the underlying `LogTable` row for the current
    // outer-loop row, resolved lazily on the first fast-path probe and
    // reused across every column scan for that row. Pre-fix every
    // column paid a fresh `MapModelIndexToLogModelRow` walk (which
    // descends each proxy layer); the mapping is constant per row, so
    // hoisting it cuts the proxy-walk cost from O(rowCount*colCount)
    // to O(rowCount) on the no-match worst case.
    auto probeCell = [&](const QModelIndex &index, int logRowCached) -> bool {
        if (useFastPath)
        {
            if (logRowCached < 0)
            {
                return false;
            }
            const std::string formatted = mLogModel->Table().GetFormattedValue(
                static_cast<size_t>(logRowCached), static_cast<size_t>(index.column())
            );
            const QString text = LogModel::ConvertToSingleLineCompactQString(formatted);
            return Matches(text, needle, flags);
        }
        const QVariant data = this->data(index, role);
        return Matches(data, value, flags);
    };

    // Sentinel: -2 means "not yet resolved this row", -1 means "tried
    // and failed (row outside the LogModel)". Both short-circuit the
    // fast path identically inside `probeCell`, but the distinction
    // lets us avoid re-running the failing map for every column.
    constexpr int LOG_ROW_UNRESOLVED = -2;

    auto resolveLogRow = [&](const QModelIndex &probeIndex, int &cache) {
        if (useFastPath && cache == LOG_ROW_UNRESOLVED)
        {
            cache = MapModelIndexToLogModelRow(probeIndex);
        }
    };

    // Outer loop step. `row` is the offset from `startRow` in the
    // forward (or backward) direction; `actualRow` is the resolved
    // row index after at most one wrap. Returns true when @p hits
    // has been reached and the caller should return immediately.
    auto scanRow = [&](int actualRow) -> bool {
        int logRowCached = LOG_ROW_UNRESOLVED;
        for (int col = 0; col < columnCount; ++col)
        {
            const int actualColumn = (startColumn + col) % columnCount;
            const QModelIndex index = this->index(actualRow, actualColumn, start.parent());
            resolveLogRow(index, logRowCached);
            if (probeCell(index, logRowCached))
            {
                result.append(index);
                if (result.size() == hits)
                {
                    return true;
                }
                break;
            }
        }
        return false;
    };

    if (forward)
    {
        for (int row = skipFirstN; row < rowCount; ++row)
        {
            int actualRow = startRow + row;
            if (actualRow >= rowCount)
            {
                // Out-of-range without wrap means we'd reuse rows
                // already past the natural boundary. Pre-fix the loop
                // implicitly wrapped via the `% rowCount` projection
                // even for `wrap=false`, so a forward search starting
                // near the tail with `skipFirstN > 0` would silently
                // visit rows below `startRow`.
                if (!wrap)
                {
                    break;
                }
                actualRow -= rowCount;
            }
            if (scanRow(actualRow))
            {
                return result;
            }
        }
    }
    else
    {
        for (int row = skipFirstN; row < rowCount; ++row)
        {
            int actualRow = startRow - row;
            if (actualRow < 0)
            {
                // Mirror of the forward branch: with `wrap=false`,
                // stop as soon as we'd cross below 0 instead of
                // wrapping to the tail. Pre-fix `skipFirstN >
                // startRow` immediately wrapped and produced matches
                // from the tail of the model.
                if (!wrap)
                {
                    break;
                }
                actualRow += rowCount;
            }
            if (scanRow(actualRow))
            {
                return result;
            }
        }
    }

    return result;
}

bool LogFilterModel::Matches(const QVariant &data, const QVariant &value, Qt::MatchFlags flags)
{
    if (flags.testFlag(Qt::MatchExactly))
    {
        return data == value;
    }
    return Matches(data.toString(), value.toString(), flags);
}

bool LogFilterModel::Matches(const QString &text, const QString &needle, Qt::MatchFlags flags)
{
    if (flags.testFlag(Qt::MatchExactly))
    {
        return text == needle;
    }
    if (flags.testFlag(Qt::MatchStartsWith))
    {
        return text.startsWith(needle);
    }
    if (flags.testFlag(Qt::MatchEndsWith))
    {
        return text.endsWith(needle);
    }
    if (flags.testFlag(Qt::MatchContains))
    {
        return text.contains(needle);
    }
    if (flags.testFlag(Qt::MatchRegularExpression))
    {
        const QRegularExpression regex(needle);
        return regex.match(text).hasMatch();
    }
    if (flags.testFlag(Qt::MatchWildcard))
    {
        const QRegularExpression regex(QRegularExpression::wildcardToRegularExpression(needle));
        return regex.match(text).hasMatch();
    }
    return false;
}
