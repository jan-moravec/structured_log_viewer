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
    // Wipe filter state before the base re-filter so predicates baked
    // against the old `LogTable` (whose dictionary the bitsets alias)
    // can't poison the new chain. Caller must re-wire via `SetLogModel`.
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
    // Skip the row-map rebuild when neither the current nor the incoming
    // rule list has anything to filter.
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
    // Predicates can't run without the table. Reaching here means rules
    // were installed without `SetLogModel` -- a bug; assert in debug,
    // reject loudly in release rather than silently accepting every row.
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
    // Production paths always wire a LogModel before sorting. The base
    // `QVariant` fallback exists for bare-proxy unit tests and the
    // transient between `setSourceModel` and the follow-up
    // `SetLogModel`. Warn once if production hits this state.
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
    // Fast path: skip the per-call `qobject_cast` via the cached
    // immediate proxy. Sorts call this O(N log N) times.
    if (mImmediateProxy != nullptr && idx.model() == mImmediateProxy)
    {
        const QModelIndex mapped = mImmediateProxy->mapToSource(idx);
        if (mapped.model() == mLogModel)
        {
            return mapped.row();
        }
        idx = mapped;
    }
    // Generic fall-through: walk deeper proxy chains.
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
    // dictionary, or attached to a different `EnumDictionary` instance
    // (covers demote -> re-promote that re-creates the entry at the
    // same `Size()`).
    if (auto it = mEnumRanks.find(lookup.canonicalKey); it != mEnumRanks.end() &&
                                                        it->second.source == lookup.dictionary &&
                                                        it->second.rank.DictSize() >= lookup.dictionary->Size())
    {
        return &it->second.rank;
    }
    // `unordered_map` value addresses are stable across rehashes; the
    // returned pointer is safe for the call's lifetime.
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

    // Fast path for `DisplayRole`: format from `LogTable` directly and
    // skip the `data().toString()` round-trip. Other roles use the
    // QVariant path (defensive; no production caller hits it today).
    const bool useFastPath = role == Qt::DisplayRole && mLogModel != nullptr;
    const QString needle = useFastPath ? value.toString() : QString{};

    // `logRowCached` is resolved once per outer-loop row and reused
    // across the column scan; the proxy mapping is row-constant.
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

    // -2 means "not yet resolved this row", -1 means "tried and failed".
    // Distinguishing them avoids re-running a failing map per column.
    constexpr int LOG_ROW_UNRESOLVED = -2;

    auto resolveLogRow = [&](const QModelIndex &probeIndex, int &cache) {
        if (useFastPath && cache == LOG_ROW_UNRESOLVED)
        {
            cache = MapModelIndexToLogModelRow(probeIndex);
        }
    };

    // Scan one row's columns. Returns true when @p hits is reached.
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
                // Without `wrap`, stop at the tail instead of cycling.
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
                // Without `wrap`, stop at the head instead of cycling.
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
