#pragma once

#include "log_model.hpp"

#include <loglib/log_compare.hpp>
#include <loglib/log_filter.hpp>

#include <QMetaObject>
#include <QSortFilterProxyModel>

#include <cstddef>
#include <memory>
#include <unordered_map>
#include <vector>

class QAbstractProxyModel;

/// Sort/filter proxy that bypasses the per-row `QVariant` round-trip.
///
/// `filterAcceptsRow` consults a vector of `loglib::RowPredicate` and
/// answers each row with one `LogTable::Get*` call plus an `EnumValueId`
/// bitset test (when armed). `lessThan` dispatches to
/// `loglib::CompareRows`, which reaches into the table by row index and
/// avoids the `data(SortRole) -> QVariant<QString>` materialisation that
/// dominated the GUI freeze on ~1 M-row enum sorts.
///
/// The proxy keeps a direct pointer to `LogModel` (set once at construction
/// time via `SetLogModel`). The intermediate `RowOrderProxyModel` is
/// traversed via `QAbstractProxyModel::mapToSource` -- this is O(1) under
/// the row-reversal layer and stays correct if more proxy layers are
/// added later.
class LogFilterModel : public QSortFilterProxyModel
{
public:
    explicit LogFilterModel(QObject *parent = nullptr);

    /// Install the underlying `LogModel`. Required before any filter
    /// rule walks rows -- predicates need direct table access.
    /// Call this *before* `setSourceModel(...)` so the
    /// `columnsMoved` invalidation hook set up there is wired against
    /// the same source-model lifetime; the production `MainWindow`
    /// wiring already follows this order. Calling `SetLogModel` after
    /// `setSourceModel` is supported (it only clears the rank cache),
    /// but the active `columnsMoved` connection remains attached to
    /// whatever source model was current at the last `setSourceModel`
    /// call -- swap source models too if the proxy chain changed.
    void SetLogModel(LogModel *logModel);

    void setSourceModel(QAbstractItemModel *sourceModel) override;

    /// Sentinel for the `hits` argument of `MatchRow`: scan to the end
    /// of the proxy without an early-stop hit count. Used by callers
    /// that want every match (e.g. "find all"); production `Find`
    /// passes `1` for first-hit semantics.
    static constexpr int UNLIMITED_HITS = -1;

    /// Look up rows in proxy-coords matching @p value (compared via
    /// `Qt::MatchFlags` against @p role). Returns the first @p hits
    /// matches, or all of them when @p hits is `UNLIMITED_HITS`.
    QList<QModelIndex> MatchRow(
        const QModelIndex &start,
        int role,
        const QVariant &value,
        int hits = 1,
        Qt::MatchFlags flags = Qt::MatchStartsWith | Qt::MatchWrap,
        bool forward = true,
        int skipFirstN = 0
    ) const;

    /// Replace the active predicate list and invalidate the proxy's
    /// row map. The vector takes ownership of the predicates.
    void SetFilterRules(std::vector<loglib::RowPredicate> &&filterRules);

    /// Drop cached `EnumDictRank` entries; the next sort comparison on
    /// an enum column will rebuild against the current dictionary.
    /// Invoked by `MainWindow` on `enumColumnsChanged`.
    void InvalidateEnumRanks();

    /// Test-only: number of cached `EnumDictRank` entries. Used by
    /// the `columnsMoved` invalidation regression test to verify the
    /// cache was dropped. Production callers must not depend on the
    /// cache shape.
    [[nodiscard]] std::size_t EnumRankCacheSizeForTest() const
    {
        return mEnumRanks.size();
    }

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;
    bool lessThan(const QModelIndex &source_left, const QModelIndex &source_right) const override;

private:
    /// Resolve the underlying `LogTable` row for @p sourceRow given
    /// @p sourceParent (which lives in the proxy's immediate `sourceModel()`
    /// coordinate system). Returns `-1` when no source model is wired.
    [[nodiscard]] int MapToLogModelRow(int sourceRow, const QModelIndex &sourceParent) const;

    /// Same as the row/parent overload but starts from an existing
    /// `QModelIndex` already living in `sourceModel()` coords. Used by
    /// `lessThan` to avoid the `index()` round-trip on the hot path.
    [[nodiscard]] int MapModelIndexToLogModelRow(QModelIndex idx) const;

    /// Refresh `mImmediateProxy` after `setSourceModel`. The cache lets
    /// `MapModelIndexToLogModelRow` skip a `qobject_cast` per call --
    /// `QSortFilterProxyModel::sort` invokes `lessThan` O(N log N) times
    /// and that cast dominates per-compare cost otherwise.
    void RefreshSourceProxyCache();

    /// Get-or-build the `EnumDictRank` for @p columnIndex. Rebuilds
    /// when the cached rank's `DictSize()` is smaller than the live
    /// dictionary (dictionary growth since the last access).
    [[nodiscard]] const loglib::EnumDictRank *EnumRankFor(int columnIndex) const;

    LogModel *mLogModel = nullptr;
    std::vector<loglib::RowPredicate> mFilterRules;

    /// Cached `qobject_cast<QAbstractProxyModel*>(sourceModel())`.
    /// Refreshed whenever the source model swaps.
    QAbstractProxyModel *mImmediateProxy = nullptr;

    /// Cached `EnumDictRank` for a column plus the live dictionary
    /// pointer it was built from. Storing the pointer lets
    /// `EnumRankFor` self-heal: if the underlying `EnumDictionary`
    /// instance changes (e.g. demote -> erase -> re-promote yields a
    /// fresh registry entry that may even have the same `Size()` as
    /// the cached rank), the cache rebuilds even when the external
    /// `InvalidateEnumRanks()` tick is missed. Pointer comparison is
    /// safe because `EnumDictionaryRegistry` keeps each dictionary at
    /// a stable address via `unique_ptr` (see `enum_dictionary.hpp`).
    struct EnumRankEntry
    {
        std::unique_ptr<const loglib::EnumDictRank> rank;
        const loglib::EnumDictionary *source = nullptr;
    };

    /// Per-column rank cache; mutable so const `lessThan` can populate
    /// lazily without leaking ownership. Cleared on:
    /// - `SetLogModel` (different `LogTable` swapped in),
    /// - `setSourceModel` (proxy chain re-wired -- the new chain may
    ///   already point at a different `LogModel`, and column indices
    ///   can have changed shape), and
    /// - the source model's `columnsMoved` signal (column reorder
    ///   shifts the per-index key, so the cached entry would attach
    ///   to the wrong column otherwise).
    /// Also invalidated piecemeal via `InvalidateEnumRanks()` on
    /// `LogModel::enumColumnsChanged`, and self-healed inside
    /// `EnumRankFor` when the cached `EnumDictionary*` no longer
    /// matches the live one.
    mutable std::unordered_map<int, EnumRankEntry> mEnumRanks;

    /// Connection to the source model's `columnsMoved` signal; reset
    /// on every `setSourceModel` swap so the cache invalidation
    /// follows the active source.
    QMetaObject::Connection mColumnsMovedConn;

    static bool Matches(const QVariant &data, const QVariant &value, Qt::MatchFlags flags);
    static bool Matches(const QString &text, const QString &needle, Qt::MatchFlags flags);
};
