#pragma once

#include "log_model.hpp"

#include <loglib/key_index.hpp>
#include <loglib/log_compare.hpp>
#include <loglib/log_filter.hpp>

#include <QSortFilterProxyModel>

#include <cstddef>
#include <unordered_map>
#include <vector>

class QAbstractProxyModel;

/// Sort/filter proxy that bypasses the per-row `QVariant` round-trip.
/// `filterAcceptsRow` runs `loglib::RowPredicate`s straight against the
/// `LogTable`; `lessThan` dispatches to `loglib::CompareRows`. Reaches the
/// table through `QAbstractProxyModel::mapToSource`, so adding more proxy
/// layers stays correct.
class LogFilterModel : public QSortFilterProxyModel
{
public:
    explicit LogFilterModel(QObject *parent = nullptr);

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
    /// `MainWindow` calls this on `enumColumnsChanged`.
    void InvalidateEnumRanks();

#ifdef LOGAPP_BUILD_TESTING
    /// Cached `EnumDictRank` count for cache-lifecycle regression tests.
    [[nodiscard]] std::size_t EnumRankCacheSizeForTest() const
    {
        return mEnumRanks.size();
    }
#endif

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &sourceLeft, const QModelIndex &sourceRight) const override;

private:
    /// Resolve the `LogTable` row for @p sourceRow / @p sourceParent.
    /// Returns `-1` when no source model is wired.
    [[nodiscard]] int MapToLogModelRow(int sourceRow, const QModelIndex &sourceParent) const;

    /// As `MapToLogModelRow` but from an existing index in `sourceModel()`
    /// coords. Skips the `index()` call on the `lessThan` hot path.
    [[nodiscard]] int MapModelIndexToLogModelRow(QModelIndex idx) const;

    /// Refresh `mImmediateProxy` after `setSourceModel`; avoids a
    /// per-`lessThan` `qobject_cast` on the O(N log N) sort path.
    void RefreshSourceProxyCache();

    /// Canonical `KeyId` for an enum column, or `INVALID_KEY_ID`.
    [[nodiscard]] loglib::KeyId EnumKeyForColumn(int columnIndex) const;

    /// Get-or-build the `EnumDictRank` for @p columnIndex. Rebuilds when
    /// the cached rank is smaller than the live dictionary or the cached
    /// `EnumDictionary*` differs (covers demote -> re-promote).
    [[nodiscard]] const loglib::EnumDictRank *EnumRankFor(int columnIndex) const;

    LogModel *mLogModel = nullptr;
    std::vector<loglib::RowPredicate> mFilterRules;

    /// Cached `qobject_cast<QAbstractProxyModel*>(sourceModel())`.
    QAbstractProxyModel *mImmediateProxy = nullptr;

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
    /// Mutable so const `lessThan` can populate lazily.
    mutable std::unordered_map<loglib::KeyId, EnumRankEntry> mEnumRanks;

    static bool Matches(const QVariant &data, const QVariant &value, Qt::MatchFlags flags);
    static bool Matches(const QString &text, const QString &needle, Qt::MatchFlags flags);
};
