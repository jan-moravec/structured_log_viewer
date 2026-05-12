#pragma once

#include "log_model.hpp"

#include <loglib/key_index.hpp>
#include <loglib/log_compare.hpp>
#include <loglib/log_filter.hpp>

#include <QAbstractProxyModel>
#include <QList>
#include <QMetaObject>
#include <QPersistentModelIndex>
#include <QString>
#include <QVariant>

#include <cstddef>
#include <unordered_map>
#include <vector>

/// Custom row-projection proxy: owns a `vector<int>` map from proxy
/// rows to source rows and rebuilds it from scratch on filter / sort
/// changes. Replaces `QSortFilterProxyModel` to skip the per-row
/// `QModelIndex` / `QVariant` round-trip on the filter and sort hot
/// paths; `RebuildAcceptedRows` evaluates `loglib::RowPredicate`s
/// straight against `loglib::LogTable`, and `sort()` permutes the
/// map via `loglib::CompareRows` with an `EnumDictRank` cache.
class LogFilterModel : public QAbstractProxyModel
{
    Q_OBJECT

public:
    explicit LogFilterModel(QObject *parent = nullptr);
    ~LogFilterModel() override;

    /// Bind the underlying `LogModel`. Required before installing any
    /// filter rule -- predicates need direct table access.
    ///
    /// Call order after every `setSourceModel`:
    ///   1. `setSourceModel(newChain)` -- clears rules + LogModel + rank
    ///      cache so stale predicates can't alias the new chain.
    ///   2. `SetLogModel(newLogModel)` -- rebind table pointer.
    ///   3. `SetFilterRules(newRules)` -- (optional) reinstall predicates.
    ///
    /// Installing rules without a `LogModel` rejects every row in release
    /// and asserts in debug.
    void SetLogModel(LogModel *logModel);

    void setSourceModel(QAbstractItemModel *sourceModel) override;

    /// Sentinel for `MatchRow`'s @p hits: return every match.
    static constexpr int UNLIMITED_HITS = -1;

    /// Look up rows in proxy-coords matching @p value against @p role.
    /// Returns up to @p hits matches (all when @p hits is `UNLIMITED_HITS`).
    QList<QModelIndex> MatchRow(
        const QModelIndex &start,
        int role,
        const QVariant &value,
        int hits = 1,
        Qt::MatchFlags flags = Qt::MatchStartsWith | Qt::MatchWrap,
        bool forward = true,
        int skipFirstN = 0
    ) const;

    /// Replace the active predicate list and invalidate the row map.
    void SetFilterRules(std::vector<loglib::RowPredicate> &&filterRules);

    /// Drop cached `EnumDictRank` entries; rebuilt lazily on next sort.
    /// `MainWindow` calls this on `enumColumnsChanged(Demoted)`.
    void InvalidateEnumRanks();

    /// Deprecated no-op kept for binary-compat with `QSortFilterProxyModel`
    /// call sites in tests and benchmarks. The new proxy reads slot types
    /// directly via `loglib::CompareRows` -- no `data(role)` round-trip --
    /// so a sort role no longer changes behaviour.
    [[deprecated("LogFilterModel sorts via loglib::CompareRows; sort role is ignored")]] void setSortRole(int /*role*/)
    {
    }

#ifdef LOGAPP_BUILD_TESTING
    /// Cached `EnumDictRank` count for cache-lifecycle regression tests.
    [[nodiscard]] std::size_t EnumRankCacheSizeForTest() const
    {
        return mEnumRanks.size();
    }
#endif

    // QAbstractProxyModel / QAbstractItemModel overrides.
    [[nodiscard]] QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QModelIndex parent(const QModelIndex &child) const override;
    [[nodiscard]] int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    [[nodiscard]] QModelIndex mapToSource(const QModelIndex &proxyIndex) const override;
    [[nodiscard]] QModelIndex mapFromSource(const QModelIndex &sourceIndex) const override;
    void sort(int column, Qt::SortOrder order = Qt::AscendingOrder) override;

private:
    /// Resolve the `LogTable` row for an index in `sourceModel()` coords
    /// by walking the proxy chain. Returns `-1` when no LogModel is bound
    /// or the chain doesn't terminate at it.
    [[nodiscard]] int SourceIndexToLogRow(QModelIndex sourceIdx) const;

    /// Walk the proxy chain from a `sourceModel()`-coords source row to
    /// the underlying `LogTable` row. `-1` on failure.
    [[nodiscard]] int SourceRowToLogRow(int sourceRow) const;

    /// Inverse of `SourceRowToLogRow`: walk the cached proxy chain via
    /// `mapFromSource` to lift a `LogTable` row up to `sourceModel()`
    /// coords. Used to map the lib-side `FilterAcceptedRows` output
    /// (log-coords indices) back into the proxy's source coords. `-1`
    /// on failure (e.g. an intermediate proxy hides the row).
    [[nodiscard]] int LogRowToSourceRow(int logRow) const;

    /// Predicate evaluation for a single source-coords row.
    [[nodiscard]] bool MatchesRulesAtSourceRow(int sourceRow) const;

    /// Compare two source-coords rows for the current sort column / order.
    /// Falls back to source-row index on ties for a deterministic order.
    [[nodiscard]] bool LessThanSourceRows(int leftSource, int rightSource) const;

    /// Replace `mAcceptedSourceRows` with the rows that survive the
    /// current `mFilterRules` against the current source. Re-applies any
    /// active sort. Emits `layoutAboutToBeChanged` / `layoutChanged` with
    /// a persistent-index remap so views keep their selection across
    /// filter / sort changes.
    void RebuildAcceptedRows();

    /// Recompute `mAcceptedSourceRows` from the current source +
    /// rules without any layout-change emit / persistent-index work.
    /// Shared by `RebuildAcceptedRows`, `OnSourceModelReset`, and
    /// `OnSourceLayoutChanged` so all three pick up
    /// `loglib::FilterAcceptedRows`'s parallel pass instead of the
    /// per-row sequential walk that used to live inline.
    void RecomputeAcceptedRows();

    /// Re-permute `mAcceptedSourceRows` for the current sort column /
    /// order. No structural emit; the caller wraps with layout signals.
    void ApplySortPermutation();

    /// Build / rebuild `mSourceRowToProxyRow` so `mapFromSource` is O(1).
    void RebuildReverseIndex();

    /// Disconnect from the previous source and connect to the current one.
    void RewireSourceConnections();

    /// Rebuild `mProxyChainAbove` from the current `sourceModel()`.
    /// Walks the chain downward through `QAbstractProxyModel::sourceModel`
    /// until it hits a non-proxy, capturing each proxy pointer in
    /// LogModel-to-sourceModel order. Empty when `sourceModel()` is
    /// the `LogModel` itself (1:1 identity, no hops needed).
    void RebuildProxyChainCache();

    /// Capture the current `persistentIndexList()` plus the source row
    /// (and column) each entry maps to, so a follow-up rebuild can
    /// remap them via `RemapPersistentIndicesForRebuild`. Caller is
    /// responsible for emitting `layoutAboutToBeChanged` first.
    void SnapshotPersistentIndices();

    /// Drop pre-rebuild persistent indices and replace them with the
    /// post-rebuild equivalents. Pairs with `SnapshotPersistentIndices`.
    void RemapPersistentIndicesForRebuild();

    /// Canonical `KeyId` for an enum column, or `INVALID_KEY_ID`.
    [[nodiscard]] loglib::KeyId EnumKeyForColumn(int columnIndex) const;

    /// Get-or-build the `EnumDictRank` for @p columnIndex. Rebuilds when
    /// the cached rank is smaller than the live dictionary or the cached
    /// `EnumDictionary*` differs (covers demote -> re-promote).
    [[nodiscard]] const loglib::EnumDictRank *EnumRankFor(int columnIndex) const;

    LogModel *mLogModel = nullptr;
    std::vector<loglib::RowPredicate> mFilterRules;

    /// Source-coords row indices, in proxy-display order. With no
    /// active sort the entries are strictly ascending; with a sort
    /// they hold the sort permutation. `mapToSource(P)` returns
    /// `sourceModel()->index(mAcceptedSourceRows[P], ...)`.
    std::vector<int> mAcceptedSourceRows;

    /// Reverse index: `mSourceRowToProxyRow[srcRow] == proxyRow` for
    /// visible rows, or `kInvisibleSourceRow` otherwise. Indexed by
    /// source row count, so `setSourceModel` / `rebuild` resize it.
    static constexpr int INVISIBLE_SOURCE_ROW = -1;
    std::vector<int> mSourceRowToProxyRow;

    /// Active sort column in source coords. `-1` means "no user sort"
    /// and `mAcceptedSourceRows` stays in ascending source-row order.
    int mSortColumn = -1;
    Qt::SortOrder mSortOrder = Qt::AscendingOrder;

    /// Tracks a successful `beginMoveColumns` from
    /// `OnSourceColumnsAboutToBeMoved` so the matching
    /// `OnSourceColumnsMoved` only calls `endMoveColumns` when the
    /// begin actually opened a pair. Qt asserts in debug if `end` is
    /// called without a matching successful `begin`.
    bool mInSourceColumnMove = false;

    /// Snapshot taken between `layoutAboutToBeChanged` and
    /// `layoutChanged`. The proxy-side persistent index pins what the
    /// view holds; the parallel source-side persistent index pins the
    /// underlying logical entity. Storing the source side as a
    /// `QPersistentModelIndex` (rather than a bare row number) lets
    /// the source's own layout-change bookkeeping follow the entity
    /// through reorders -- e.g. `RowOrderProxyModel::SetReversed`
    /// remaps source indices via `changePersistentIndexList`, so the
    /// post-rebuild lookup naturally lands on the same entity rather
    /// than on whichever entity now sits at the pre-flip row number.
    /// Pinned by `TestNewestFirstReversalPreservesFilterModelSelection`.
    QList<QPersistentModelIndex> mPersistentIndexSnapshot;
    QList<QPersistentModelIndex> mPersistentSourceIndexSnapshot;

    /// Cached rank plus the dictionary pointer it was built from.
    /// `EnumRankFor` rebuilds when the pointer changes -- covers
    /// demote -> re-promote that re-creates the registry entry.
    struct EnumRankEntry
    {
        loglib::EnumDictRank rank;
        const loglib::EnumDictionary *source = nullptr;
    };

    /// Per-column rank cache keyed by canonical `KeyId` so it survives
    /// column reorders without a `columnsMoved` hook. Cleared by
    /// `SetLogModel`, `setSourceModel`, and `InvalidateEnumRanks`;
    /// self-healed inside `EnumRankFor` when the cached
    /// `EnumDictionary*` no longer matches the live one.
    /// Mutable so const `sort`-time helpers can populate lazily.
    mutable std::unordered_map<loglib::KeyId, EnumRankEntry> mEnumRanks;

    /// Signal connections to the current source. Cleared and rebuilt
    /// in `setSourceModel`.
    std::vector<QMetaObject::Connection> mSourceConnections;

    /// Proxy chain above `LogModel`, ordered LogModel -> sourceModel.
    /// `LogRowToSourceRow` walks this via `mapFromSource` once per
    /// accepted row to lift log-coords indices to source coords.
    /// Empty when `sourceModel() == mLogModel` (test wiring). Cached
    /// so we don't repeat the downward walk per call.
    std::vector<QAbstractProxyModel *> mProxyChainAbove;

    // Source-signal handlers.
    void OnSourceRowsInserted(const QModelIndex &parent, int first, int last);
    void OnSourceRowsAboutToBeRemoved(const QModelIndex &parent, int first, int last);
    void OnSourceRowsRemoved(const QModelIndex &parent, int first, int last);
    void OnSourceDataChanged(const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles);
    void OnSourceModelAboutToBeReset();
    void OnSourceModelReset();
    void OnSourceLayoutAboutToBeChanged();
    void OnSourceLayoutChanged();
    void OnSourceColumnsInserted(const QModelIndex &parent, int first, int last);
    void OnSourceColumnsRemoved(const QModelIndex &parent, int first, int last);
    void OnSourceColumnsAboutToBeMoved(
        const QModelIndex &parent, int from, int toLast, const QModelIndex &dest, int destColumn
    );
    void OnSourceColumnsMoved(const QModelIndex &parent, int from, int toLast, const QModelIndex &dest, int destColumn);
    void OnSourceHeaderDataChanged(Qt::Orientation orientation, int first, int last);

    static bool Matches(const QVariant &data, const QVariant &value, Qt::MatchFlags flags);
    static bool Matches(const QString &text, const QString &needle, Qt::MatchFlags flags);
};
