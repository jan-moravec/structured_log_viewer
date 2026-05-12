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

    // Wipe filter state before re-wiring so predicates baked against the
    // old table's dictionary can't poison the new chain. Caller must
    // re-bind via `SetLogModel` before installing rules.
    mFilterRules.clear();
    mLogModel = nullptr;
    mEnumRanks.clear();
    mAcceptedSourceRows.clear();
    mSourceRowToProxyRow.clear();
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
    QAbstractItemModel *src = sourceModel();
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
    // `column == -1` clears any user sort and returns to ascending
    // source-row order; mirrors `QSortFilterProxyModel::sort(-1)`.
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
    if (mSortColumn < 0 || mLogModel == nullptr)
    {
        return;
    }
    // Stable sort: ties resolve by source-row index, mirroring the
    // historical `setSortRole(SortRole)` + insertion-order tie-break.
    std::ranges::stable_sort(mAcceptedSourceRows, [this](int lhs, int rhs) { return LessThanSourceRows(lhs, rhs); });
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
    QAbstractItemModel *src = sourceModel();
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
    // Walk the proxy chain. Mirrors the prior `MapModelIndexToLogModelRow`
    // shape but enters at a source-coords index, not a proxy-coords one.
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

bool LogFilterModel::MatchesRulesAtSourceRow(int sourceRow) const
{
    if (mFilterRules.empty())
    {
        return true;
    }
    if (mLogModel == nullptr)
    {
        // Predicates can't run without the table. Reaching here means
        // rules were installed without `SetLogModel`; assert in debug,
        // reject loudly in release rather than silently accepting.
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

void LogFilterModel::RebuildAcceptedRows()
{
    QAbstractItemModel *src = sourceModel();
    if (src == nullptr)
    {
        return;
    }

    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    SnapshotPersistentIndices();

    const int n = src->rowCount();
    mAcceptedSourceRows.clear();
    mAcceptedSourceRows.reserve(static_cast<size_t>(n));
    if (mFilterRules.empty())
    {
        // Empty rule list: every source row passes regardless of
        // LogModel binding (no predicate has to evaluate).
        for (int i = 0; i < n; ++i)
        {
            mAcceptedSourceRows.push_back(i);
        }
    }
    else if (mLogModel == nullptr)
    {
        // Rules without a `LogModel`: predicates can't evaluate. Assert
        // in debug, reject every row in release (loud failure mode is
        // preferable to silently accepting everything). Pinned by
        // `TestFilterAcceptsRowRejectsAllWithoutLogModel`.
        Q_ASSERT_X(
            false,
            "LogFilterModel::RebuildAcceptedRows",
            "filter rules set without a LogModel; call SetLogModel before SetFilterRules"
        );
        static std::once_flag warnedNoLogModelFlag;
        std::call_once(warnedNoLogModelFlag, [] {
            qWarning() << "LogFilterModel: filter rules present but mLogModel is null; rejecting every row";
        });
        // Leave `mAcceptedSourceRows` empty.
    }
    else
    {
        for (int i = 0; i < n; ++i)
        {
            if (MatchesRulesAtSourceRow(i))
            {
                mAcceptedSourceRows.push_back(i);
            }
        }
    }
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
    mPersistentSourceRowSnapshot.clear();
    mPersistentColumnSnapshot.clear();
    mPersistentSourceRowSnapshot.reserve(static_cast<size_t>(raw.size()));
    mPersistentColumnSnapshot.reserve(static_cast<size_t>(raw.size()));
    for (const QModelIndex &idx : raw)
    {
        mPersistentIndexSnapshot.append(QPersistentModelIndex{idx});
        if (idx.isValid() && static_cast<size_t>(idx.row()) < mAcceptedSourceRows.size())
        {
            mPersistentSourceRowSnapshot.push_back(mAcceptedSourceRows[static_cast<size_t>(idx.row())]);
            mPersistentColumnSnapshot.push_back(idx.column());
        }
        else
        {
            mPersistentSourceRowSnapshot.push_back(-1);
            mPersistentColumnSnapshot.push_back(-1);
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
        mPersistentSourceRowSnapshot.clear();
        mPersistentColumnSnapshot.clear();
        return;
    }
    QModelIndexList replacements;
    replacements.reserve(mPersistentIndexSnapshot.size());
    for (qsizetype i = 0; i < mPersistentIndexSnapshot.size(); ++i)
    {
        const int srcRow = mPersistentSourceRowSnapshot[static_cast<size_t>(i)];
        const int col = mPersistentColumnSnapshot[static_cast<size_t>(i)];
        if (srcRow < 0 || col < 0)
        {
            replacements.append(QModelIndex{});
            continue;
        }
        if (static_cast<size_t>(srcRow) >= mSourceRowToProxyRow.size())
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
    mPersistentSourceRowSnapshot.clear();
    mPersistentColumnSnapshot.clear();
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

    // Shift existing accepted entries whose source row was >= first up
    // by `insertedCount`. With no active sort, `mAcceptedSourceRows` is
    // ascending: shift the tail in-place.
    for (int &row : mAcceptedSourceRows)
    {
        if (row >= first)
        {
            row += insertedCount;
        }
    }

    // Append-test the new source rows. In streaming mode `first` equals
    // the old row count, so most predicate work happens here.
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
        // No user sort: keep ascending order. Insert each new row at
        // its lower_bound position; for tail appends (`r >= max
        // existing source row`) this degenerates to a back-insert.
        for (const int r : newlyAccepted)
        {
            const auto it = std::ranges::lower_bound(mAcceptedSourceRows, r);
            const int proxyRow = static_cast<int>(std::distance(mAcceptedSourceRows.begin(), it));
            beginInsertRows(QModelIndex{}, proxyRow, proxyRow);
            mAcceptedSourceRows.insert(it, r);
            endInsertRows();
        }
        RebuildReverseIndex();
    }
    else if (!newlyAccepted.empty())
    {
        // Active sort: lower_bound on the comparator gives the correct
        // insertion point per row. Emit per-row inserts so views keep
        // their selection / scroll position.
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
        RebuildReverseIndex();
    }
    else if (!mAcceptedSourceRows.empty() || sourceModel()->rowCount() > 0)
    {
        // Source grew but no row passed the filter; still keep the
        // reverse index sized to the new source row count.
        RebuildReverseIndex();
    }
    else
    {
        RebuildReverseIndex();
    }
}

void LogFilterModel::OnSourceRowsAboutToBeRemoved(
    const QModelIndex & /*parent*/, int /*first*/, int /*last*/
)
{
    // Removal is handled in `OnSourceRowsRemoved` (the post-event
    // slot) so we can run begin/endRemoveRows synchronously per
    // contiguous proxy-row strike. The pre-event slot stays connected
    // so the signal pair is symmetric and so future work can snapshot
    // persistent indices here without re-wiring.
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

    // Two-pass: first emit `beginRemoveRows` for each contiguous proxy
    // range that maps into [first, last]; then rebuild the row map
    // with surviving entries shifted down by the source eviction.
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
    // descending order so each `beginRemoveRows` argument is valid
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

    for (auto it = ranges.rbegin(); it != ranges.rend(); ++it)
    {
        const auto [proxyFirst, proxyLast] = *it;
        beginRemoveRows(QModelIndex{}, proxyFirst, proxyLast);
        mAcceptedSourceRows.erase(
            mAcceptedSourceRows.begin() + proxyFirst, mAcceptedSourceRows.begin() + proxyLast + 1
        );
        endRemoveRows();
    }

    // Shift remaining source-row entries down by `removedCount` if
    // they were past `last`.
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

    // Forward as a proxy `dataChanged` over the proxy rows that map
    // into the affected source range. Conservative: if any active
    // filter rule targets a column in the changed range, rebuild the
    // row map so newly-accepted / newly-rejected rows reflect.
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

    // Emit a proxy `dataChanged` covering only the rows in the
    // affected source range that are currently visible.
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
        const int n = sourceModel()->rowCount();
        mAcceptedSourceRows.reserve(static_cast<size_t>(n));
        if (mFilterRules.empty())
        {
            for (int i = 0; i < n; ++i)
            {
                mAcceptedSourceRows.push_back(i);
            }
        }
        else if (mLogModel != nullptr)
        {
            for (int i = 0; i < n; ++i)
            {
                if (MatchesRulesAtSourceRow(i))
                {
                    mAcceptedSourceRows.push_back(i);
                }
            }
        }
        // else: rules but no LogModel -> leave empty (reject all).
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
    // Capture persistent index snapshot so the post-layout-change
    // rebuild can remap. RowOrderProxyModel's reversal toggle is the
    // common source-layout event we handle here.
    emit layoutAboutToBeChanged({}, QAbstractItemModel::VerticalSortHint);
    SnapshotPersistentIndices();
}

void LogFilterModel::OnSourceLayoutChanged()
{
    // After a source layoutChanged the source-row meaning may have
    // shifted (RowOrderProxyModel reversal toggle remaps every row).
    // Rebuild without a fresh layoutAboutToBeChanged emit; the
    // OnSourceLayoutAboutToBeChanged slot already paired one.
    QAbstractItemModel *src = sourceModel();
    if (src == nullptr)
    {
        // No-op: just complete the layoutChanged bracket.
        RemapPersistentIndicesForRebuild();
        emit layoutChanged({}, QAbstractItemModel::VerticalSortHint);
        return;
    }
    const int n = src->rowCount();
    mAcceptedSourceRows.clear();
    mAcceptedSourceRows.reserve(static_cast<size_t>(n));
    if (mFilterRules.empty())
    {
        for (int i = 0; i < n; ++i)
        {
            mAcceptedSourceRows.push_back(i);
        }
    }
    else if (mLogModel != nullptr)
    {
        for (int i = 0; i < n; ++i)
        {
            if (MatchesRulesAtSourceRow(i))
            {
                mAcceptedSourceRows.push_back(i);
            }
        }
    }
    // else: rules but no LogModel -> leave empty (reject all).
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

void LogFilterModel::OnSourceColumnsMoved(
    const QModelIndex & /*parent*/, int from, int toLast, const QModelIndex & /*dest*/, int destRow
)
{
    // Track the sort column through the move. We deliberately don't
    // re-permute `mAcceptedSourceRows` because the row map is
    // row-indexed; only the active sort column index needs adjusting.
    const int span = toLast - from + 1;
    if (mSortColumn >= 0)
    {
        if (mSortColumn >= from && mSortColumn <= toLast)
        {
            mSortColumn = destRow + (mSortColumn - from);
            if (destRow > toLast)
            {
                mSortColumn -= span;
            }
        }
        else if (mSortColumn > toLast && mSortColumn < destRow)
        {
            mSortColumn -= span;
        }
        else if (mSortColumn >= destRow && mSortColumn < from)
        {
            mSortColumn += span;
        }
    }
    // Forward the layout-style move by relaying as a full reset of
    // column headers; persistent indices stay valid because we don't
    // touch row state. `headerDataChanged` is the simplest way to
    // refresh the column header text in the view without juggling
    // begin/end pairs whose semantics differ from QAbstractItemModel's.
    emit headerDataChanged(Qt::Horizontal, 0, columnCount() - 1);
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
    // (covers demote -> re-promote that re-creates the entry at the
    // same `Size()`).
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
    // skip the `data().toString()` round-trip. Other roles use the
    // QVariant path (defensive; no production caller hits it today).
    const bool useFastPath = role == Qt::DisplayRole && mLogModel != nullptr;
    const QString needle = useFastPath ? value.toString() : QString{};

    // `logRowCached` is resolved once per outer-loop row and reused
    // across the column scan; the proxy mapping is row-constant.
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

    auto scanRow = [&](int actualRow) -> bool {
        int logRowCached = LOG_ROW_UNRESOLVED;
        for (int col = 0; col < columnCount; ++col)
        {
            const int actualColumn = (startColumn + col) % columnCount;
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
