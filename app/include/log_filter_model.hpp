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
#include <functional>
#include <unordered_map>
#include <vector>

/// Row-projection proxy. Holds a `vector<int>` mapping proxy rows to
/// source rows and rebuilds it from scratch on filter / sort changes.
/// Replaces `QSortFilterProxyModel` to skip the per-row `QModelIndex`
/// / `QVariant` round-trip: `RebuildAcceptedRows` evaluates
/// `loglib::RowPredicate`s straight against `loglib::LogTable`, and
/// `sort()` permutes the map via `loglib::CompareRows` with an
/// `EnumDictRank` cache.
class LogFilterModel : public QAbstractProxyModel
{
    Q_OBJECT

public:
    explicit LogFilterModel(QObject *parent = nullptr);
    ~LogFilterModel() override;

    /// Bind the `LogModel` whose `LogTable` predicates evaluate against.
    /// Required before installing any filter rule.
    ///
    /// Call order after `setSourceModel`:
    ///   1. `setSourceModel(newChain)` -- clears rules, model, rank cache.
    ///   2. `SetLogModel(newLogModel)` -- rebind table pointer.
    ///   3. `SetFilterRules(newRules)` -- (optional) reinstall predicates.
    ///
    /// Installing rules without a `LogModel` asserts in debug and rejects
    /// every row in release.
    void SetLogModel(LogModel *logModel);

    void setSourceModel(QAbstractItemModel *sourceModel) override;

    /// Sentinel `hits` value meaning "return every match".
    static constexpr int UNLIMITED_HITS = -1;

    /// Build `Qt::MatchFlags` for an incremental find query.
    ///
    /// Single source of truth for find call sites: the match-type
    /// values in `Qt::MatchFlag` are alternatives (not bit-mask
    /// modifiers), so OR-ing `MatchContains` with `MatchWildcard` /
    /// `MatchRegularExpression` silently demotes to substring matching.
    /// Always sets `MatchWrap | MatchRecursive`; picks exactly one of
    /// regex / wildcard / contains based on the UI toggles.
    [[nodiscard]] static Qt::MatchFlags ComposeFindFlags(bool wildcards, bool regularExpressions);

    /// Find proxy-coord rows whose cell matches @p value under @p role.
    /// Returns up to @p hits matches. `hits` must be either
    /// `UNLIMITED_HITS` (return every match) or `>= 1`; `0` is
    /// rejected via `Q_ASSERT` (asking for zero results is a
    /// programming error, and the wrapper's stop-condition
    /// `size < hits` would otherwise stop after the first match
    /// under `hits == 0`, which is a silent behaviour change
    /// from the previous "0 == all" alias — force callers to
    /// pick a real value instead).
    QList<QModelIndex> MatchRow(
        const QModelIndex &start,
        int role,
        const QVariant &value,
        int hits = 1,
        Qt::MatchFlags flags = Qt::MatchStartsWith | Qt::MatchWrap,
        bool forward = true,
        int skipFirstN = 0
    ) const;

    /// Callback invoked once per matching proxy-coord row in
    /// `ForEachMatchingRow`. Returning `true` continues the walk;
    /// returning `false` stops it. Only one cell per row is
    /// reported (the first matching column in scan order), mirroring
    /// `MatchRow`.
    using MatchRowCallback = std::function<bool(const QModelIndex &proxyIndex)>;

    /// Iterate matching proxy-coord rows via a callback rather than
    /// materialising them into a `QList<QModelIndex>`. Same probe
    /// semantics as `MatchRow` (hidden columns skipped, `MatchWrap`
    /// honoured, `DisplayRole` fast path through `LogTable`), but
    /// avoids the O(matches) list allocation when the caller only
    /// needs to accumulate aggregates (per-bucket counters, total
    /// count). `MatchRow` is now a thin wrapper on top of this.
    void ForEachMatchingRow(
        const QModelIndex &start,
        int role,
        const QVariant &value,
        Qt::MatchFlags flags,
        bool forward,
        int skipFirstN,
        const MatchRowCallback &onMatch
    ) const;

    /// Replace the active predicate list and rebuild the row map.
    void SetFilterRules(std::vector<loglib::RowPredicate> &&filterRules);

    /// Drop cached `EnumDictRank` entries; lazily rebuilt on next sort.
    /// `MainWindow` calls this on `enumColumnsChanged(Demoted)`.
    void InvalidateEnumRanks();

    /// Active sort column (source coords); `-1` if no user sort is
    /// installed. Pairs with `SortOrder()` for the session-save
    /// mirror.
    [[nodiscard]] int SortColumn() const noexcept
    {
        return mSortColumn;
    }
    [[nodiscard]] Qt::SortOrder SortOrder() const noexcept
    {
        return mSortOrder;
    }

    /// Deprecated no-op. The proxy reads slot types directly via
    /// `loglib::CompareRows`, so the sort role is ignored. Kept so
    /// existing tests / benchmarks compile.
    [[deprecated("LogFilterModel sorts via loglib::CompareRows; sort role is ignored")]] void setSortRole(int /*role*/)
    {
    }

#ifdef LOGAPP_BUILD_TESTING
    /// Cached `EnumDictRank` entry count. Used by cache-lifecycle tests.
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
    /// Walk the proxy chain from a `sourceModel()`-coords index down to
    /// the underlying `LogTable` row. `-1` if no `LogModel` is bound
    /// or the chain doesn't terminate at it.
    [[nodiscard]] int SourceIndexToLogRow(QModelIndex sourceIdx) const;

    /// Row-overload of `SourceIndexToLogRow`. `-1` on failure.
    [[nodiscard]] int SourceRowToLogRow(int sourceRow) const;

    /// Inverse of `SourceRowToLogRow`: lift a `LogTable` row up to
    /// `sourceModel()` coords by walking `mProxyChainAbove` via
    /// `mapFromSource`. Used to map `FilterAcceptedRows`'s log-coord
    /// output back to source coords. `-1` if any proxy hides the row.
    [[nodiscard]] int LogRowToSourceRow(int logRow) const;

    /// Predicate evaluation for one source-coords row.
    [[nodiscard]] bool MatchesRulesAtSourceRow(int sourceRow) const;

    /// Compare two source-coords rows under the active sort column /
    /// order. Ties fall back to source-row index for determinism.
    [[nodiscard]] bool LessThanSourceRows(int leftSource, int rightSource) const;

    /// Refresh `mAcceptedSourceRows` from the current source + rules,
    /// re-apply any active sort, and emit `layoutAboutToBeChanged` /
    /// `layoutChanged` with a persistent-index remap so views keep
    /// their selection.
    void RebuildAcceptedRows();

    /// Refresh `mAcceptedSourceRows` only -- no layout emit, no
    /// persistent-index work. Shared by `RebuildAcceptedRows`,
    /// `OnSourceModelReset`, and `OnSourceLayoutChanged` so all three
    /// go through `loglib::FilterAcceptedRows`'s parallel pass.
    void RecomputeAcceptedRows();

    /// Re-permute `mAcceptedSourceRows` for the active sort column /
    /// order. No structural emit; caller brackets with layout signals.
    void ApplySortPermutation();

    /// Rebuild `mSourceRowToProxyRow` so `mapFromSource` is O(1).
    void RebuildReverseIndex();

    /// Disconnect from the previous source and connect to the current one.
    void RewireSourceConnections();

    /// Rebuild `mProxyChainAbove` by walking downward from `sourceModel()`
    /// through `QAbstractProxyModel::sourceModel` until a non-proxy
    /// (the `LogModel`). Empty when `sourceModel() == mLogModel`.
    void RebuildProxyChainCache();

    /// Capture `persistentIndexList()` and the source row/column each
    /// entry maps to so a follow-up rebuild can remap them via
    /// `RemapPersistentIndicesForRebuild`. Caller must have emitted
    /// `layoutAboutToBeChanged` first.
    void SnapshotPersistentIndices();

    /// Replace the snapshotted persistent indices with their
    /// post-rebuild equivalents. Pairs with `SnapshotPersistentIndices`.
    void RemapPersistentIndicesForRebuild();

    /// Canonical `KeyId` for an enum column, or `INVALID_KEY_ID`.
    [[nodiscard]] loglib::KeyId EnumKeyForColumn(int columnIndex) const;

    /// Get-or-build the `EnumDictRank` for @p columnIndex. Rebuilds
    /// when the cached rank is smaller than the live dictionary or
    /// its `EnumDictionary*` differs (covers demote -> re-promote).
    [[nodiscard]] const loglib::EnumDictRank *EnumRankFor(int columnIndex) const;

    LogModel *mLogModel = nullptr;
    std::vector<loglib::RowPredicate> mFilterRules;

    /// Source-coord row indices in proxy-display order. Ascending
    /// without an active sort; holds the sort permutation otherwise.
    /// `mapToSource(P)` returns `sourceModel()->index(mAcceptedSourceRows[P], ...)`.
    std::vector<int> mAcceptedSourceRows;

    /// Reverse index: `mSourceRowToProxyRow[srcRow] == proxyRow` for
    /// visible rows, `INVISIBLE_SOURCE_ROW` otherwise. Resized whenever
    /// the source row count changes.
    static constexpr int INVISIBLE_SOURCE_ROW = -1;
    std::vector<int> mSourceRowToProxyRow;

    /// Active sort column in source coords. `-1` means "no user sort";
    /// `mAcceptedSourceRows` then stays in ascending source-row order.
    int mSortColumn = -1;
    Qt::SortOrder mSortOrder = Qt::AscendingOrder;

    /// Tracks whether `OnSourceColumnsAboutToBeMoved` successfully
    /// opened a `beginMoveColumns` pair, so `OnSourceColumnsMoved`
    /// only calls `endMoveColumns` when there's a matching `begin`.
    bool mInSourceColumnMove = false;

    /// Snapshots taken between `layoutAboutToBeChanged` and
    /// `layoutChanged`. The proxy-side index pins what the view holds;
    /// the source-side `QPersistentModelIndex` pins the underlying
    /// entity. Storing the source side persistently (not as a bare row
    /// number) lets it follow the entity through source-layout
    /// reorders -- e.g. `RowOrderProxyModel::SetReversed`. Pinned by
    /// `TestNewestFirstReversalPreservesFilterModelSelection`.
    QList<QPersistentModelIndex> mPersistentIndexSnapshot;
    QList<QPersistentModelIndex> mPersistentSourceIndexSnapshot;

    /// Cached rank + the dictionary pointer it was built from.
    /// `EnumRankFor` rebuilds when the pointer changes, which covers
    /// demote -> re-promote re-creating the registry entry.
    struct EnumRankEntry
    {
        loglib::EnumDictRank rank;
        const loglib::EnumDictionary *source = nullptr;
    };

    /// Per-column rank cache, keyed on canonical `KeyId` so it survives
    /// column reorders. Cleared by `SetLogModel`, `setSourceModel`, and
    /// `InvalidateEnumRanks`; self-heals inside `EnumRankFor` when the
    /// cached `EnumDictionary*` differs from the live one. Mutable so
    /// const sort-time helpers can populate it lazily.
    mutable std::unordered_map<loglib::KeyId, EnumRankEntry> mEnumRanks;

    /// Signal connections to the current source. Refreshed by `setSourceModel`.
    std::vector<QMetaObject::Connection> mSourceConnections;

    /// Proxy chain above `LogModel`, ordered LogModel -> sourceModel.
    /// `LogRowToSourceRow` walks this via `mapFromSource`. Empty when
    /// `sourceModel() == mLogModel` (test wiring). Cached so we don't
    /// repeat the downward walk per call.
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
