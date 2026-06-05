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
#include <iterator>
#include <mutex>
#include <ranges>
#include <span>
#include <utility>

LogFilterModel::LogFilterModel(QObject *parent)
    : QAbstractProxyModel{parent}
{
}

LogFilterModel::~LogFilterModel() = default;

void LogFilterModel::SetLogModel(LogModel *logModel)
{
    mLogModel = logModel;
    mEnumRanks.clear();
    if (sourceModel() != nullptr)
    {
        RebuildAcceptedRows();
    }
}

void LogFilterModel::setSourceModel(QAbstractItemModel *sourceModel)
{
    beginResetModel();

    // Wipe filter state before rewiring: predicates baked against the
    // old table's dictionary must not leak into the new chain. Caller
    // re-binds via `SetLogModel` before installing rules.
    mFilterRules.clear();
    mLogModel = nullptr;
    mEnumRanks.clear();
    mAcceptedSourceRows.clear();
    mSourceRowToProxyRow.clear();
    mProxyChainAbove.clear();
    mSortColumn = -1;
    mSortOrder = Qt::AscendingOrder;

    for (const QMetaObject::Connection &c : mSourceConnections)
    {
        QObject::disconnect(c);
    }
    mSourceConnections.clear();

    QAbstractProxyModel::setSourceModel(sourceModel);

    if (sourceModel != nullptr)
    {
        RewireSourceConnections();
        RebuildProxyChainCache();
        const int n = sourceModel->rowCount();
        mAcceptedSourceRows.resize(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
        {
            mAcceptedSourceRows[static_cast<size_t>(i)] = i;
        }
        RebuildReverseIndex();
    }

    endResetModel();
}

void LogFilterModel::RewireSourceConnections()
{
    const QAbstractItemModel *src = sourceModel();
    if (src == nullptr)
    {
        return;
    }
    mSourceConnections.push_back(
        connect(src, &QAbstractItemModel::rowsInserted, this, &LogFilterModel::OnSourceRowsInserted)
    );
    mSourceConnections.push_back(
        connect(src, &QAbstractItemModel::rowsAboutToBeRemoved, this, &LogFilterModel::OnSourceRowsAboutToBeRemoved)
    );
    mSourceConnections.push_back(
        connect(src, &QAbstractItemModel::rowsRemoved, this, &LogFilterModel::OnSourceRowsRemoved)
    );
    mSourceConnections.push_back(
        connect(src, &QAbstractItemModel::dataChanged, this, &LogFilterModel::OnSourceDataChanged)
    );
    mSourceConnections.push_back(
        connect(src, &QAbstractItemModel::modelAboutToBeReset, this, &LogFilterModel::OnSourceModelAboutToBeReset)
    );
    mSourceConnections.push_back(
        connect(src, &QAbstractItemModel::modelReset, this, &LogFilterModel::OnSourceModelReset)
    );
    mSourceConnections.push_back(
        connect(src, &QAbstractItemModel::layoutAboutToBeChanged, this, &LogFilterModel::OnSourceLayoutAboutToBeChanged)
    );
    mSourceConnections.push_back(
        connect(src, &QAbstractItemModel::layoutChanged, this, &LogFilterModel::OnSourceLayoutChanged)
    );
    mSourceConnections.push_back(
        connect(src, &QAbstractItemModel::columnsInserted, this, &LogFilterModel::OnSourceColumnsInserted)
    );
    mSourceConnections.push_back(
        connect(src, &QAbstractItemModel::columnsRemoved, this, &LogFilterModel::OnSourceColumnsRemoved)
    );
    mSourceConnections.push_back(
        connect(src, &QAbstractItemModel::columnsAboutToBeMoved, this, &LogFilterModel::OnSourceColumnsAboutToBeMoved)
    );
    mSourceConnections.push_back(
        connect(src, &QAbstractItemModel::columnsMoved, this, &LogFilterModel::OnSourceColumnsMoved)
    );
    mSourceConnections.push_back(
        connect(src, &QAbstractItemModel::headerDataChanged, this, &LogFilterModel::OnSourceHeaderDataChanged)
    );
}

void LogFilterModel::SetFilterRules(std::vector<loglib::RowPredicate> &&filterRules)
{
    if (mFilterRules.empty() && filterRules.empty())
    {
        return;
    }
    mFilterRules = std::move(filterRules);
    RebuildAcceptedRows();
}

void LogFilterModel::InvalidateEnumRanks()
{
    mEnumRanks.clear();
}

QModelIndex LogFilterModel::index(int row, int column, const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return {};
    }
    if (row < 0 || column < 0)
    {
        return {};
    }
    if (static_cast<size_t>(row) >= mAcceptedSourceRows.size())
    {
        return {};
    }
    if (sourceModel() == nullptr || column >= sourceModel()->columnCount())
    {
        return {};
    }
    return createIndex(row, column);
}

QModelIndex LogFilterModel::parent(const QModelIndex & /*child*/) const
{
    return {};
}

int LogFilterModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
    {
        return 0;
    }
    return static_cast<int>(mAcceptedSourceRows.size());
}

int LogFilterModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid() || sourceModel() == nullptr)
    {
        return 0;
    }
    return sourceModel()->columnCount();
}

QModelIndex LogFilterModel::mapToSource(const QModelIndex &proxyIndex) const
{
    if (!proxyIndex.isValid() || sourceModel() == nullptr)
    {
        return {};
    }
    const int proxyRow = proxyIndex.row();
    if (proxyRow < 0 || static_cast<size_t>(proxyRow) >= mAcceptedSourceRows.size())
    {
        return {};
    }
    return sourceModel()->index(mAcceptedSourceRows[static_cast<size_t>(proxyRow)], proxyIndex.column());
}

QModelIndex LogFilterModel::mapFromSource(const QModelIndex &sourceIndex) const
{
    if (!sourceIndex.isValid() || sourceModel() == nullptr)
    {
        return {};
    }
    const int srcRow = sourceIndex.row();
    if (srcRow < 0 || static_cast<size_t>(srcRow) >= mSourceRowToProxyRow.size())
    {
        return {};
    }
    const int proxyRow = mSourceRowToProxyRow[static_cast<size_t>(srcRow)];
    if (proxyRow == INVISIBLE_SOURCE_ROW)
    {
        return {};
    }
    return createIndex(proxyRow, sourceIndex.column());
}

void LogFilterModel::sort(int column, Qt::SortOrder order)
{
    if (sourceModel() == nullptr)
    {
        mSortColumn = column;
        mSortOrder = order;
        return;
    }
    // `column == -1` clears the user sort and reverts to ascending
    // source-row order, matching `QSortFilterProxyModel::sort(-1)`.
    if (column < 0)
    {
        if (mSortColumn < 0)
        {
            return;
        }
        emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
        SnapshotPersistentIndices();

        mSortColumn = -1;
        mSortOrder = order;
        std::ranges::sort(mAcceptedSourceRows);
        RebuildReverseIndex();

        RemapPersistentIndicesForRebuild();
        emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
        return;
    }

    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    SnapshotPersistentIndices();

    mSortColumn = column;
    mSortOrder = order;
    ApplySortPermutation();
    RebuildReverseIndex();

    RemapPersistentIndicesForRebuild();
    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

void LogFilterModel::ApplySortPermutation()
{
    if (mSortColumn < 0 || mLogModel == nullptr || mAcceptedSourceRows.size() <= 1)
    {
        return;
    }
    const auto &columns = mLogModel->Configuration().columns;
    if (static_cast<size_t>(mSortColumn) >= columns.size())
    {
        return;
    }

    // Resolve every source row to its log row once. The old
    // comparator-driven path walked the proxy chain twice per
    // `lessThan` call (~40 M walks on a 1 M-row enum sort, dominating
    // wall-clock). With pre-resolution the sort comparator stays
    // inside `loglib` and never touches a `QModelIndex`.
    std::vector<size_t> logRows;
    logRows.reserve(mAcceptedSourceRows.size());
    for (const int srcRow : mAcceptedSourceRows)
    {
        const int logRow = SourceRowToLogRow(srcRow);
        // Every entry in `mAcceptedSourceRows` was pushed by a path
        // that already resolved the log mapping. A stray `-1` here
        // means the proxy chain emitted a structural change between
        // the accept-list rebuild and this sort -- assert loudly. In
        // release we collapse the offender onto log row 0; combined
        // with the `loglib::CompareRows` tail-bucket invariant that
        // still produces a defined position rather than corrupting
        // the parallel arrays. Pinned by `TestApplySortPermutation*`.
        Q_ASSERT_X(
            logRow >= 0, "LogFilterModel::ApplySortPermutation", "source row failed to resolve to a log row mid-sort"
        );
        logRows.push_back(logRow >= 0 ? static_cast<size_t>(logRow) : size_t{0});
    }

    const loglib::EnumDictRank *rank = nullptr;
    if (columns[static_cast<size_t>(mSortColumn)].type == loglib::LogConfiguration::Type::Enumeration)
    {
        rank = EnumRankFor(mSortColumn);
    }

    const std::vector<size_t> permutation = loglib::SortPermutationByColumn(
        mLogModel->Table(),
        std::span<const size_t>{logRows},
        static_cast<size_t>(mSortColumn),
        mSortOrder == Qt::AscendingOrder,
        rank
    );

    std::vector<int> sorted;
    sorted.reserve(mAcceptedSourceRows.size());
    for (const size_t idx : permutation)
    {
        sorted.push_back(mAcceptedSourceRows[idx]);
    }
    mAcceptedSourceRows = std::move(sorted);
}

bool LogFilterModel::LessThanSourceRows(int leftSource, int rightSource) const
{
    if (mLogModel == nullptr)
    {
        return leftSource < rightSource;
    }
    const auto &columns = mLogModel->Configuration().columns;
    if (mSortColumn < 0 || static_cast<size_t>(mSortColumn) >= columns.size())
    {
        return leftSource < rightSource;
    }
    const int leftLog = SourceRowToLogRow(leftSource);
    const int rightLog = SourceRowToLogRow(rightSource);
    if (leftLog < 0 || rightLog < 0)
    {
        return leftSource < rightSource;
    }
    const loglib::EnumDictRank *rank = nullptr;
    if (columns[static_cast<size_t>(mSortColumn)].type == loglib::LogConfiguration::Type::Enumeration)
    {
        rank = EnumRankFor(mSortColumn);
    }
    const int cmp = loglib::CompareRows(
        mLogModel->Table(),
        static_cast<size_t>(leftLog),
        static_cast<size_t>(rightLog),
        static_cast<size_t>(mSortColumn),
        rank
    );
    if (cmp == 0)
    {
        return leftSource < rightSource;
    }
    return mSortOrder == Qt::AscendingOrder ? cmp < 0 : cmp > 0;
}

int LogFilterModel::SourceRowToLogRow(int sourceRow) const
{
    const QAbstractItemModel *src = sourceModel();
    if (src == nullptr || sourceRow < 0)
    {
        return -1;
    }
    return SourceIndexToLogRow(src->index(sourceRow, 0));
}

int LogFilterModel::SourceIndexToLogRow(QModelIndex sourceIdx) const
{
    if (!sourceIdx.isValid() || mLogModel == nullptr)
    {
        return -1;
    }
    // Walk the proxy chain. Same shape as the prior
    // `MapModelIndexToLogModelRow`, but entered at a source-coords index.
    while (const auto *proxy = qobject_cast<const QAbstractProxyModel *>(sourceIdx.model()))
    {
        const QModelIndex mapped = proxy->mapToSource(sourceIdx);
        if (!mapped.isValid())
        {
            return -1;
        }
        sourceIdx = mapped;
    }
    if (sourceIdx.model() != mLogModel)
    {
        return -1;
    }
    return sourceIdx.row();
}

int LogFilterModel::LogRowToSourceRow(int logRow) const
{
    if (mLogModel == nullptr || sourceModel() == nullptr || logRow < 0)
    {
        return -1;
    }
    // Empty chain: `sourceModel() == mLogModel`. The "log row" is
    // already the "source row" -- no proxy hop required.
    // Empty chain (`sourceModel() == mLogModel`): the log row is the
    // source row.
    if (mProxyChainAbove.empty())
    {
        return logRow;
    }
    QModelIndex idx = mLogModel->index(logRow, 0);
    if (!idx.isValid())
    {
        return -1;
    }
    for (const QAbstractProxyModel *proxy : mProxyChainAbove)
    {
        idx = proxy->mapFromSource(idx);
        if (!idx.isValid())
        {
            return -1;
        }
    }
    return idx.row();
}

void LogFilterModel::RebuildProxyChainCache()
{
    mProxyChainAbove.clear();
    // Walk downward through `sourceModel()` until we hit a non-proxy
    // (the `LogModel`). Reverse so the cache reads bottom-up
    // (LogModel -> sourceModel), the order `mapFromSource` walks need.
    QAbstractItemModel *cursor = sourceModel();
    while (auto *proxy = qobject_cast<QAbstractProxyModel *>(cursor))
    {
        mProxyChainAbove.push_back(proxy);
        cursor = proxy->sourceModel();
    }
    std::ranges::reverse(mProxyChainAbove);
}

bool LogFilterModel::MatchesRulesAtSourceRow(int sourceRow) const
{
    if (mFilterRules.empty())
    {
        return true;
    }
    if (mLogModel == nullptr)
    {
        // Rules were installed without `SetLogModel`. Assert in debug;
        // in release reject every row (loud failure beats silent accept).
        Q_ASSERT_X(
            mLogModel != nullptr,
            "LogFilterModel::MatchesRulesAtSourceRow",
            "filter rules set without a LogModel; call SetLogModel before SetFilterRules"
        );
        static std::once_flag warnedNoLogModelFlag;
        std::call_once(warnedNoLogModelFlag, [] {
            qWarning() << "LogFilterModel: filter rules present but mLogModel is null; rejecting every row";
        });
        return false;
    }
    const int logRow = SourceRowToLogRow(sourceRow);
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

void LogFilterModel::RecomputeAcceptedRows()
{
    const QAbstractItemModel *src = sourceModel();
    mAcceptedSourceRows.clear();
    if (src == nullptr)
    {
        return;
    }

    const int n = src->rowCount();
    mAcceptedSourceRows.reserve(static_cast<size_t>(n));
    if (mFilterRules.empty())
    {
        // No rules: every row passes, even without a `LogModel`.
        for (int i = 0; i < n; ++i)
        {
            mAcceptedSourceRows.push_back(i);
        }
        return;
    }
    if (mLogModel == nullptr)
    {
        // Rules without a `LogModel`: predicates can't evaluate. Assert
        // in debug, reject every row in release (loud failure beats
        // silent accept). Pinned by `TestFilterAcceptsRowRejectsAllWithoutLogModel`.
        Q_ASSERT_X(
            false,
            "LogFilterModel::RecomputeAcceptedRows",
            "filter rules set without a LogModel; call SetLogModel before SetFilterRules"
        );
        static std::once_flag warnedNoLogModelFlag;
        std::call_once(warnedNoLogModelFlag, [] {
            qWarning() << "LogFilterModel: filter rules present but mLogModel is null; rejecting every row";
        });
        return;
    }

    // Hot path: hand predicate evaluation off to
    // `loglib::FilterAcceptedRows`, which runs `tbb::parallel_for` over
    // `LogTable` rows directly. The lib has no Qt dependency and no
    // proxy-chain awareness, so we map each surviving log row back to
    // source coords here.
    const auto acceptedLogRows = loglib::FilterAcceptedRows(mLogModel->Table(), std::span{mFilterRules});
    mAcceptedSourceRows.reserve(acceptedLogRows.size());
    for (const size_t logRow : acceptedLogRows)
    {
        const int srcRow = LogRowToSourceRow(static_cast<int>(logRow));
        if (srcRow >= 0)
        {
            mAcceptedSourceRows.push_back(srcRow);
        }
    }
    // The lib returns log rows in ascending order, but the proxy chain
    // may permute them (e.g. `RowOrderProxyModel` reverses). Restore
    // ascending source-row order so the reverse index and later
    // streaming inserts can rely on it. Order-preserving chains pay
    // an already-sorted pass.
    std::ranges::sort(mAcceptedSourceRows);
}

void LogFilterModel::RebuildAcceptedRows()
{
    const QAbstractItemModel *src = sourceModel();
    if (src == nullptr)
    {
        return;
    }

    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    SnapshotPersistentIndices();

    RecomputeAcceptedRows();
    if (mSortColumn >= 0)
    {
        ApplySortPermutation();
    }
    RebuildReverseIndex();

    RemapPersistentIndicesForRebuild();
    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

void LogFilterModel::SnapshotPersistentIndices()
{
    const QModelIndexList raw = persistentIndexList();
    mPersistentIndexSnapshot.clear();
    mPersistentIndexSnapshot.reserve(raw.size());
    mPersistentSourceIndexSnapshot.clear();
    mPersistentSourceIndexSnapshot.reserve(raw.size());
    const QAbstractItemModel *src = sourceModel();
    for (const QModelIndex &idx : raw)
    {
        mPersistentIndexSnapshot.append(QPersistentModelIndex{idx});
        if (src != nullptr && idx.isValid() && static_cast<size_t>(idx.row()) < mAcceptedSourceRows.size())
        {
            const int srcRow = mAcceptedSourceRows[static_cast<size_t>(idx.row())];
            // Anchor on a source-side persistent index (not a bare row
            // number). When the source reshuffles its own rows
            // (e.g. `RowOrderProxyModel::SetReversed`) it remaps every
            // persistent index via `changePersistentIndexList`, so
            // this anchor keeps pointing at the same entity. For sort
            // / filter rebuilds the source layout doesn't move, so
            // the anchor's row is unchanged and the behaviour collapses
            // to the previous "stored source row number" path.
            mPersistentSourceIndexSnapshot.append(QPersistentModelIndex{src->index(srcRow, idx.column())});
        }
        else
        {
            mPersistentSourceIndexSnapshot.append(QPersistentModelIndex{});
        }
    }
}

void LogFilterModel::RebuildReverseIndex()
{
    const int srcCount = sourceModel() != nullptr ? sourceModel()->rowCount() : 0;
    mSourceRowToProxyRow.assign(static_cast<size_t>(srcCount), INVISIBLE_SOURCE_ROW);
    for (size_t proxyRow = 0; proxyRow < mAcceptedSourceRows.size(); ++proxyRow)
    {
        const int srcRow = mAcceptedSourceRows[proxyRow];
        if (srcRow >= 0 && static_cast<size_t>(srcRow) < mSourceRowToProxyRow.size())
        {
            mSourceRowToProxyRow[static_cast<size_t>(srcRow)] = static_cast<int>(proxyRow);
        }
    }
}

void LogFilterModel::RemapPersistentIndicesForRebuild()
{
    if (mPersistentIndexSnapshot.isEmpty())
    {
        mPersistentSourceIndexSnapshot.clear();
        return;
    }
    QModelIndexList replacements;
    replacements.reserve(mPersistentIndexSnapshot.size());
    for (qsizetype i = 0; i < mPersistentIndexSnapshot.size(); ++i)
    {
        const QPersistentModelIndex &srcAnchor = mPersistentSourceIndexSnapshot[i];
        if (!srcAnchor.isValid())
        {
            replacements.append(QModelIndex{});
            continue;
        }
        // Read the post-rebuild row / column out of the source anchor.
        // After a sort / filter rebuild the source layout didn't move
        // and this matches the original snapshot; after a source-side
        // layout change the anchor was updated by the source's own
        // `changePersistentIndexList` pass and now points where the
        // entity moved to.
        const int srcRow = srcAnchor.row();
        const int col = srcAnchor.column();
        if (srcRow < 0 || col < 0 || static_cast<size_t>(srcRow) >= mSourceRowToProxyRow.size())
        {
            replacements.append(QModelIndex{});
            continue;
        }
        const int proxyRow = mSourceRowToProxyRow[static_cast<size_t>(srcRow)];
        if (proxyRow == INVISIBLE_SOURCE_ROW)
        {
            replacements.append(QModelIndex{});
            continue;
        }
        replacements.append(createIndex(proxyRow, col));
    }
    QModelIndexList oldList;
    oldList.reserve(mPersistentIndexSnapshot.size());
    for (const QPersistentModelIndex &p : mPersistentIndexSnapshot)
    {
        oldList.append(QModelIndex{p});
    }
    changePersistentIndexList(oldList, replacements);
    mPersistentIndexSnapshot.clear();
    mPersistentSourceIndexSnapshot.clear();
}

void LogFilterModel::OnSourceRowsInserted(const QModelIndex &parent, int first, int last)
{
    if (parent.isValid() || sourceModel() == nullptr)
    {
        return;
    }
    const int insertedCount = last - first + 1;
    if (insertedCount <= 0)
    {
        return;
    }

    // Shift accepted entries whose source row was >= first up by
    // `insertedCount`. Under no sort, `mAcceptedSourceRows` is
    // ascending so this is an in-place tail shift. Under a sort, the
    // comparator reads `SourceRowToLogRow(srcRow)`; the source's
    // row->log mapping shifts in lockstep with the entity, so each
    // shifted entry still describes the same entity in the same order.
    for (int &row : mAcceptedSourceRows)
    {
        if (row >= first)
        {
            row += insertedCount;
        }
    }

    // Probe the new source rows. In streaming mode `first` is the old
    // row count, so most predicate work happens here.
    std::vector<int> newlyAccepted;
    newlyAccepted.reserve(static_cast<size_t>(insertedCount));
    for (int r = first; r <= last; ++r)
    {
        if (MatchesRulesAtSourceRow(r))
        {
            newlyAccepted.push_back(r);
        }
    }

    if (mSortColumn < 0)
    {
        // No sort: `mAcceptedSourceRows` stays ascending. The shift
        // above left a contiguous "gap" at `[first, first+insertedCount-1]`,
        // so every newly accepted row lands between the entries `<first`
        // and the shifted entries `>= first+insertedCount`. One
        // `beginInsertRows`/`endInsertRows` bracket covers the whole
        // group.
        //
        // Matters during streaming: a 50 k-row batch with no filter
        // and no sort used to emit 50 k bracketed signal pairs and
        // 50 k `vector::insert` shifts. The bulk path is O(1) brackets
        // and one tail append. Pinned by
        // `TestStreamingAppendsEmitSingleBracketedInsert`.
        if (!newlyAccepted.empty())
        {
            const auto insertIt = std::ranges::lower_bound(mAcceptedSourceRows, first);
            const int proxyFirst = static_cast<int>(std::distance(mAcceptedSourceRows.begin(), insertIt));
            const int proxyLast = proxyFirst + static_cast<int>(newlyAccepted.size()) - 1;
            beginInsertRows(QModelIndex{}, proxyFirst, proxyLast);
            mAcceptedSourceRows.insert(insertIt, newlyAccepted.begin(), newlyAccepted.end());
            // Refresh the reverse index inside the bracket so observers
            // calling `mapFromSource` from a `rowsInserted` slot see a
            // consistent model. (The per-row branch below rebuilds
            // outside the bracket -- intermediate states would be
            // momentarily stale either way.)
            RebuildReverseIndex();
            endInsertRows();
            return;
        }
        // Source grew without contributing an accepted row: still
        // resize the reverse index so out-of-range `mapFromSource`
        // queries don't read past the old size.
        RebuildReverseIndex();
        return;
    }

    if (!newlyAccepted.empty())
    {
        // Active sort: `lower_bound` on the comparator picks each
        // insertion point. We emit per-row inserts so views keep their
        // selection / scroll position. Coalescing the inserts is not
        // worth the complexity for "streaming-while-sorted" (users
        // typically sort after streaming completes).
        for (const int r : newlyAccepted)
        {
            const auto it = std::ranges::lower_bound(mAcceptedSourceRows, r, [this](int lhs, int rhs) {
                return LessThanSourceRows(lhs, rhs);
            });
            const int proxyRow = static_cast<int>(std::distance(mAcceptedSourceRows.begin(), it));
            beginInsertRows(QModelIndex{}, proxyRow, proxyRow);
            mAcceptedSourceRows.insert(it, r);
            endInsertRows();
        }
    }
    // The reverse index is sized off the source row count: refresh
    // whenever the source grew, even if no row passed the filter.
    RebuildReverseIndex();
}

void LogFilterModel::OnSourceRowsAboutToBeRemoved(
    const QModelIndex & /*parent*/, int /*first*/, int /*last*/
)
{
    // Removal is handled in the post-event `OnSourceRowsRemoved` so
    // we can run begin/endRemoveRows synchronously per contiguous
    // proxy-row strike. Keep this slot connected so the signal pair
    // stays symmetric and so future work can snapshot persistent
    // indices here without rewiring.
}

void LogFilterModel::OnSourceRowsRemoved(const QModelIndex &parent, int first, int last)
{
    if (parent.isValid() || sourceModel() == nullptr)
    {
        return;
    }
    const int removedCount = last - first + 1;
    if (removedCount <= 0)
    {
        return;
    }

    // Two-pass: emit `beginRemoveRows` for each contiguous proxy range
    // that maps into [first, last], then shift surviving entries down
    // by the source eviction.
    std::vector<int> proxyRowsToDrop;
    proxyRowsToDrop.reserve(static_cast<size_t>(removedCount));
    for (size_t i = 0; i < mAcceptedSourceRows.size(); ++i)
    {
        const int sr = mAcceptedSourceRows[i];
        if (sr >= first && sr <= last)
        {
            proxyRowsToDrop.push_back(static_cast<int>(i));
        }
    }

    // Coalesce into contiguous proxy-row ranges and emit removes in
    // descending order, so each `beginRemoveRows` argument is valid
    // against the row count that exists when it fires.
    auto coalesceRanges = [&proxyRowsToDrop]() {
        std::vector<std::pair<int, int>> ranges;
        if (proxyRowsToDrop.empty())
        {
            return ranges;
        }
        int rangeStart = proxyRowsToDrop.front();
        int rangeEnd = rangeStart;
        for (size_t i = 1; i < proxyRowsToDrop.size(); ++i)
        {
            const int p = proxyRowsToDrop[i];
            if (p == rangeEnd + 1)
            {
                rangeEnd = p;
            }
            else
            {
                ranges.emplace_back(rangeStart, rangeEnd);
                rangeStart = p;
                rangeEnd = p;
            }
        }
        ranges.emplace_back(rangeStart, rangeEnd);
        return ranges;
    };
    std::vector<std::pair<int, int>> ranges = coalesceRanges();

    for (const auto [proxyFirst, proxyLast] : std::views::reverse(ranges))
    {
        beginRemoveRows(QModelIndex{}, proxyFirst, proxyLast);
        mAcceptedSourceRows.erase(
            mAcceptedSourceRows.begin() + proxyFirst, mAcceptedSourceRows.begin() + proxyLast + 1
        );
        endRemoveRows();
    }

    // Shift surviving entries whose source row was past `last` down
    // by `removedCount`.
    for (int &sr : mAcceptedSourceRows)
    {
        if (sr > last)
        {
            sr -= removedCount;
        }
    }
    RebuildReverseIndex();
}

void LogFilterModel::OnSourceDataChanged(
    const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles
)
{
    if (sourceModel() == nullptr)
    {
        return;
    }
    if (!topLeft.isValid() || !bottomRight.isValid())
    {
        return;
    }

    // Forward as a proxy `dataChanged` over the proxy rows in the
    // affected source range. Conservative: if any active rule targets
    // a column in the changed range, rebuild the row map so newly
    // accepted / rejected rows take effect.
    const int srcColFirst = topLeft.column();
    const int srcColLast = bottomRight.column();
    bool filterTargetsChangedColumn = false;
    for (const auto &rule : mFilterRules)
    {
        const auto column = static_cast<int>(loglib::RowPredicateColumn(rule));
        if (column >= srcColFirst && column <= srcColLast)
        {
            filterTargetsChangedColumn = true;
            break;
        }
    }
    if (filterTargetsChangedColumn)
    {
        RebuildAcceptedRows();
        return;
    }

    // Emit a proxy `dataChanged` covering only the currently visible
    // proxy rows that fall in the affected source range.
    const int srcFirst = topLeft.row();
    const int srcLast = bottomRight.row();
    int proxyFirst = -1;
    int proxyLast = -1;
    for (int sr = srcFirst; sr <= srcLast; ++sr)
    {
        if (sr < 0 || static_cast<size_t>(sr) >= mSourceRowToProxyRow.size())
        {
            continue;
        }
        const int pr = mSourceRowToProxyRow[static_cast<size_t>(sr)];
        if (pr == INVISIBLE_SOURCE_ROW)
        {
            continue;
        }
        proxyFirst = proxyFirst < 0 ? pr : std::min(proxyFirst, pr);
        proxyLast = proxyLast < 0 ? pr : std::max(proxyLast, pr);
    }
    if (proxyFirst >= 0)
    {
        emit dataChanged(index(proxyFirst, srcColFirst), index(proxyLast, srcColLast), roles);
    }
}

void LogFilterModel::OnSourceModelAboutToBeReset()
{
    beginResetModel();
}

void LogFilterModel::OnSourceModelReset()
{
    mAcceptedSourceRows.clear();
    mSourceRowToProxyRow.clear();
    mEnumRanks.clear();
    if (sourceModel() != nullptr)
    {
        // Route through the parallel-filter helper. A sequential
        // per-row walk here would pay proxy-chain costs the rest of
        // the proxy has already escaped (felt on 1 M-row reloads).
        RecomputeAcceptedRows();
        if (mSortColumn >= 0)
        {
            ApplySortPermutation();
        }
        RebuildReverseIndex();
    }
    endResetModel();
}

void LogFilterModel::OnSourceLayoutAboutToBeChanged()
{
    // Snapshot persistent indices so the matching `layoutChanged`
    // slot can remap them. `RowOrderProxyModel::SetReversed` is the
    // common production trigger.
    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    SnapshotPersistentIndices();
}

void LogFilterModel::OnSourceLayoutChanged()
{
    // The source may have reshuffled rows (e.g. `RowOrderProxyModel`
    // reversal). Rebuild without re-emitting `layoutAboutToBeChanged`;
    // `OnSourceLayoutAboutToBeChanged` already paired one.
    const QAbstractItemModel *src = sourceModel();
    if (src == nullptr)
    {
        // No-op: just close the layoutChanged bracket.
        RemapPersistentIndicesForRebuild();
        emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
        return;
    }
    // Reuse the parallel-filter path. A sequential per-row walk here
    // gave a perf regression any time an upstream proxy emitted
    // `layoutChanged` (e.g. the newest-first toggle on large logs).
    RecomputeAcceptedRows();
    if (mSortColumn >= 0)
    {
        ApplySortPermutation();
    }
    RebuildReverseIndex();
    RemapPersistentIndicesForRebuild();
    emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
}

void LogFilterModel::OnSourceColumnsInserted(const QModelIndex &parent, int first, int last)
{
    if (parent.isValid())
    {
        return;
    }
    beginInsertColumns(QModelIndex{}, first, last);
    if (mSortColumn >= first)
    {
        mSortColumn += (last - first + 1);
    }
    endInsertColumns();
}

void LogFilterModel::OnSourceColumnsRemoved(const QModelIndex &parent, int first, int last)
{
    if (parent.isValid())
    {
        return;
    }
    beginRemoveColumns(QModelIndex{}, first, last);
    if (mSortColumn >= first && mSortColumn <= last)
    {
        mSortColumn = -1;
    }
    else if (mSortColumn > last)
    {
        mSortColumn -= (last - first + 1);
    }
    endRemoveColumns();
}

void LogFilterModel::OnSourceColumnsAboutToBeMoved(
    const QModelIndex &parent, int from, int toLast, const QModelIndex &dest, int destColumn
)
{
    // Forward the begin/end column-move pair so downstream views
    // (header, persistent indices, delegates) see a properly
    // bracketed structural change.
    //
    // Hierarchical moves and Qt-refused begins leave
    // `mInSourceColumnMove == false`; the post-move slot then skips
    // both `endMoveColumns` and the `mSortColumn` adjustment so we
    // never speak about a move that didn't happen at our level.
    if (parent.isValid() || dest.isValid())
    {
        mInSourceColumnMove = false;
        return;
    }
    mInSourceColumnMove = beginMoveColumns(QModelIndex{}, from, toLast, QModelIndex{}, destColumn);
}

void LogFilterModel::OnSourceColumnsMoved(
    const QModelIndex & /*parent*/, int from, int toLast, const QModelIndex & /*dest*/, int destColumn
)
{
    // Track the sort column through the move. We don't re-permute
    // `mAcceptedSourceRows` -- the row map is row-indexed; only the
    // sort column index needs adjusting.
    //
    // Gate on `mInSourceColumnMove`: hierarchical moves at the source
    // layer carry their own from/toLast/destColumn coords that aren't
    // meaningful to our top-level column list, and adjusting against
    // them would silently corrupt the sort index.
    if (mInSourceColumnMove)
    {
        const int span = toLast - from + 1;
        if (mSortColumn >= 0)
        {
            if (mSortColumn >= from && mSortColumn <= toLast)
            {
                mSortColumn = destColumn + (mSortColumn - from);
                if (destColumn > toLast)
                {
                    mSortColumn -= span;
                }
            }
            else if (mSortColumn > toLast && mSortColumn < destColumn)
            {
                mSortColumn -= span;
            }
            else if (mSortColumn >= destColumn && mSortColumn < from)
            {
                mSortColumn += span;
            }
        }
        endMoveColumns();
        mInSourceColumnMove = false;
    }
}

void LogFilterModel::OnSourceHeaderDataChanged(Qt::Orientation orientation, int first, int last)
{
    emit headerDataChanged(orientation, first, last);
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
    // (the last case covers demote -> re-promote that re-creates the
    // registry entry at the same `Size()`).
    if (auto it = mEnumRanks.find(lookup.canonicalKey); it != mEnumRanks.end() &&
                                                        it->second.source == lookup.dictionary &&
                                                        it->second.rank.DictSize() >= lookup.dictionary->Size())
    {
        return &it->second.rank;
    }
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
    // skip the `data().toString()` round-trip. Other roles fall through
    // to the QVariant path (defensive; no production caller hits it today).
    const bool useFastPath = role == Qt::DisplayRole && mLogModel != nullptr;
    const QString needle = useFastPath ? value.toString() : QString{};

    // `logRowCached` is resolved once per row and reused across the
    // column scan (the proxy mapping is row-constant).
    auto probeCell = [&](const QModelIndex &proxyIndex, int logRowCached) -> bool {
        if (useFastPath && logRowCached >= 0)
        {
            const std::string formatted = mLogModel->Table().GetFormattedValue(
                static_cast<size_t>(logRowCached), static_cast<size_t>(proxyIndex.column())
            );
            const QString text = LogModel::ConvertToSingleLineCompactQString(formatted);
            return Matches(text, needle, flags);
        }
        const QVariant data = this->data(proxyIndex, role);
        return Matches(data, value, flags);
    };

    constexpr int LOG_ROW_UNRESOLVED = -2;

    auto resolveLogRow = [&](const QModelIndex &probeProxyIndex, int &cache) {
        if (useFastPath && cache == LOG_ROW_UNRESOLVED)
        {
            cache = SourceIndexToLogRow(mapToSource(probeProxyIndex));
        }
    };

    // Skip hidden columns: a hit on an invisible cell scrolls the
    // row into view but leaves the user with no idea what matched
    // (zero-width section). Source of truth is `Column::visible`.
    // No-op when `mLogModel` is null (tests without a config).
    const auto *columnsForVisibility = mLogModel != nullptr ? &mLogModel->Configuration().columns : nullptr;
    auto isColumnHidden = [columnsForVisibility](int column) {
        if (columnsForVisibility == nullptr)
        {
            return false;
        }
        const auto idx = static_cast<size_t>(column);
        return idx < columnsForVisibility->size() && !(*columnsForVisibility)[idx].visible;
    };

    auto scanRow = [&](int actualRow) -> bool {
        int logRowCached = LOG_ROW_UNRESOLVED;
        for (int col = 0; col < columnCount; ++col)
        {
            const int actualColumn = (startColumn + col) % columnCount;
            if (isColumnHidden(actualColumn))
            {
                continue;
            }
            const QModelIndex proxyIndex = this->index(actualRow, actualColumn, start.parent());
            resolveLogRow(proxyIndex, logRowCached);
            if (probeCell(proxyIndex, logRowCached))
            {
                result.append(proxyIndex);
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
    // `Qt::MatchExactly == 0`, so `flags.testFlag(Qt::MatchExactly)`
    // returns true only when `flags` is exactly 0 -- every caller in
    // this codebase OR-s in `MatchWrap | MatchRecursive`, so the
    // direct-equality branch was historically dead code. Mask the
    // match-type field (see the QString overload below for the
    // disjoint-bit rationale) and short-circuit MatchExactly on the
    // QVariant equality, which is cheaper than stringifying twice and
    // preserves typed comparisons (numeric, timestamp, enum id).
    constexpr int MATCH_TYPE_MASK = 0x000F;
    const int matchType = static_cast<int>(flags) & MATCH_TYPE_MASK;
    if (matchType == Qt::MatchExactly)
    {
        return data == value;
    }
    return Matches(data.toString(), value.toString(), flags);
}

bool LogFilterModel::Matches(const QString &text, const QString &needle, Qt::MatchFlags flags)
{
    // `Qt::MatchFlag`'s match-type values are *not* disjoint bit
    // flags: `MatchWildcard = 5` overlaps `MatchContains = 1` and
    // `MatchRegularExpression = 4`; `MatchEndsWith = 3` overlaps
    // `MatchContains = 1` and `MatchStartsWith = 2`. A naive
    // `testFlag` ladder therefore picks the *first* check whose
    // bit subset is present, which is silently the *least*
    // specific match -- so `MatchWildcard` would fall into the
    // `MatchContains` branch and the wildcard search would run
    // as a plain substring scan with the literal `*` / `?`. Mask
    // the match-type field and compare for exact equality so the
    // semantics line up with how Qt's own item-view searches
    // interpret these constants. The QVariant overload above uses
    // the same mask so the two stay in lock-step.
    constexpr int MATCH_TYPE_MASK = 0x000F;
    const int matchType = static_cast<int>(flags) & MATCH_TYPE_MASK;
    switch (matchType)
    {
    case Qt::MatchExactly:
        return text == needle;
    case Qt::MatchStartsWith:
        return text.startsWith(needle);
    case Qt::MatchEndsWith:
        return text.endsWith(needle);
    case Qt::MatchContains:
        return text.contains(needle);
    case Qt::MatchRegularExpression:
    {
        const QRegularExpression regex(needle);
        return regex.match(text).hasMatch();
    }
    case Qt::MatchWildcard:
    {
        const QRegularExpression regex(QRegularExpression::wildcardToRegularExpression(needle));
        return regex.match(text).hasMatch();
    }
    default:
        return false;
    }
}
