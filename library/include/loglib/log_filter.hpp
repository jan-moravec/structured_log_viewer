#pragma once

#include "loglib/enum_dictionary.hpp"
#include "loglib/internal/transparent_string_hash.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <variant>
#include <vector>

namespace loglib
{

class LogTable;

/// Multi-select equality predicate for `Type::Enumeration` columns.
/// Hot path: one `GetEnumValueId` + `vector<bool>` test. Rows whose
/// slot is non-`DictRef` (column not yet promoted) fall back to a
/// transparent-hash string set.
///
/// The bitset is a snapshot of the dictionary at construction.
/// Callers should rebuild the predicate on `enumColumnsChanged`.
/// Stale predicates still work: an id past the bitset rejects when
/// `mAllResolved`, otherwise falls through to the string set.
///
/// Threading: today every caller runs on the GUI thread. The
/// past-bitset branch keeps the predicate correct even if a future
/// caller races dictionary growth.
class EnumRowPredicate
{
public:
    EnumRowPredicate(
        size_t columnIndex, std::span<const std::string_view> selectedValues, const EnumDictionary *dictionary
    );

    EnumRowPredicate(const EnumRowPredicate &) = delete;
    EnumRowPredicate &operator=(const EnumRowPredicate &) = delete;
    EnumRowPredicate(EnumRowPredicate &&) noexcept = default;
    EnumRowPredicate &operator=(EnumRowPredicate &&) noexcept = default;
    ~EnumRowPredicate() = default;

    [[nodiscard]] bool MatchesRow(const LogTable &table, size_t row) const;

    /// Column index this predicate targets, in `LogTable` coords.
    [[nodiscard]] size_t ColumnIndex() const noexcept
    {
        return mColumnIndex;
    }

    /// True iff the bitset has at least one armed bit.
    [[nodiscard]] bool IsFastPathArmed() const noexcept
    {
        return mFastPathArmed;
    }

private:
    size_t mColumnIndex = 0;
    /// Indexed by `EnumValueId`. Empty when no dictionary was given.
    std::vector<bool> mSelectedIds;
    /// Selected values that did not resolve at construction (or all
    /// of them when no dictionary was given). Skipped on the fully-
    /// resolved fast path; covers unpromoted slots and stale-predicate
    /// past-bitset hits.
    std::unordered_set<std::string, internal::TransparentStringHash, internal::TransparentStringEqual> mSelectedStrings;
    bool mFastPathArmed = false;
    /// Every selected value resolved to an id at construction.
    bool mAllResolved = false;
    /// Constructor was given an empty selection; `MatchesRow` rejects
    /// every row. Named sentinel guards against future field additions
    /// breaking the obvious inference.
    bool mEmptySelection = false;
};

/// Inclusive time-range predicate. Bounds are microseconds since the
/// UNIX epoch. An inverted range (`begin > end`) rejects every row.
class TimeRangeRowPredicate
{
public:
    TimeRangeRowPredicate(size_t columnIndex, int64_t begin, int64_t end);

    TimeRangeRowPredicate(const TimeRangeRowPredicate &) = default;
    TimeRangeRowPredicate &operator=(const TimeRangeRowPredicate &) = default;
    TimeRangeRowPredicate(TimeRangeRowPredicate &&) noexcept = default;
    TimeRangeRowPredicate &operator=(TimeRangeRowPredicate &&) noexcept = default;
    ~TimeRangeRowPredicate() = default;

    [[nodiscard]] bool MatchesRow(const LogTable &table, size_t row) const;

    /// Column index this predicate targets, in `LogTable` coords.
    [[nodiscard]] size_t ColumnIndex() const noexcept
    {
        return mColumnIndex;
    }

private:
    size_t mColumnIndex = 0;
    int64_t mBegin = 0;
    int64_t mEnd = 0;
};

/// String predicate that defers matching to a caller-supplied
/// callback. Lets the GUI keep Qt-flavoured regex / wildcard semantics
/// without lib pulling in a Qt dependency. The caller owns callback
/// thread-safety; the GUI builder pre-JITs its `QRegularExpression`
/// so captured copies don't re-compile lazily.
class CallbackStringRowPredicate
{
public:
    using MatchFn = std::function<bool(std::string_view)>;

    CallbackStringRowPredicate(size_t columnIndex, MatchFn match);

    CallbackStringRowPredicate(const CallbackStringRowPredicate &) = default;
    CallbackStringRowPredicate &operator=(const CallbackStringRowPredicate &) = default;
    CallbackStringRowPredicate(CallbackStringRowPredicate &&) noexcept = default;
    CallbackStringRowPredicate &operator=(CallbackStringRowPredicate &&) noexcept = default;
    ~CallbackStringRowPredicate() = default;

    [[nodiscard]] bool MatchesRow(const LogTable &table, size_t row) const;

    /// Column index this predicate targets, in `LogTable` coords.
    [[nodiscard]] size_t ColumnIndex() const noexcept
    {
        return mColumnIndex;
    }

private:
    size_t mColumnIndex = 0;
    MatchFn mMatch;
};

/// Closed-set union of concrete row predicates. The GUI proxy stores
/// these by value -- no heap allocation per rule, no virtual dispatch
/// on the per-row hot path.
using RowPredicate = std::variant<EnumRowPredicate, TimeRangeRowPredicate, CallbackStringRowPredicate>;

/// Visit-dispatch helper; resolves to the concrete `MatchesRow` at
/// compile time.
[[nodiscard]] inline bool MatchesRow(const RowPredicate &predicate, const LogTable &table, size_t row)
{
    return std::visit([&table, row](const auto &concrete) { return concrete.MatchesRow(table, row); }, predicate);
}

/// Column index targeted by @p predicate. Used by the GUI proxy to
/// decide whether a source `dataChanged` requires a filter rebuild.
[[nodiscard]] inline size_t RowPredicateColumn(const RowPredicate &predicate) noexcept
{
    return std::visit([](const auto &concrete) noexcept { return concrete.ColumnIndex(); }, predicate);
}

/// Evaluate @p predicates against every row in @p table in parallel and
/// return the row indices that pass every predicate, in ascending order.
/// Empty @p predicates returns `[0, table.RowCount())` (degenerate
/// identity case; the GUI proxy short-circuits before calling).
///
/// Threading: each worker accumulates surviving rows into a
/// thread-local bucket via `tbb::parallel_for`; buckets are coalesced
/// and sorted on the calling thread before return. Predicate
/// `MatchesRow` implementations must be thread-safe read-only against
/// @p table -- the three predicates in this file qualify
/// (`CallbackStringRowPredicate` formats into a `thread_local` buffer,
/// `EnumRowPredicate` reads an immutable bitset snapshot,
/// `TimeRangeRowPredicate` is stateless). The function intentionally
/// lives in `loglib` rather than the GUI proxy so callers do not need
/// a TBB include in their translation unit.
[[nodiscard]] std::vector<size_t> FilterAcceptedRows(const LogTable &table, std::span<const RowPredicate> predicates);

} // namespace loglib
