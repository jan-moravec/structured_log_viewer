#pragma once

#include "loglib/enum_dictionary.hpp"
#include "loglib/internal/transparent_string_hash.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
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
/// Hot path: one `GetEnumValueId` + `vector<bool>` test. Rows with a
/// non-`DictRef` slot (column not yet promoted) fall back to a
/// transparent-hash string set.
///
/// The bitset is a dictionary snapshot taken at construction. Callers
/// should rebuild the predicate on `enumColumnsChanged`. Stale
/// predicates still work: an id past the bitset rejects when
/// `mAllResolved`, otherwise falls through to the string set.
///
/// Threading: `MatchesRow` is read-only and stateless on `*this`
/// (the constructor writes `mSelectedIds` / `mSelectedStrings` and
/// nobody mutates them afterwards). `FilterAcceptedRows` invokes it
/// concurrently from `tbb::parallel_for`. The past-bitset branch
/// keeps results correct even if a writer grows the dictionary
/// mid-evaluation: growth only pushes new ids past the bitset's
/// `size()`, and the string-set fallback handles them.
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
    /// resolved fast path; covers unpromoted slots and past-bitset
    /// hits from stale predicates.
    std::unordered_set<std::string, internal::TransparentStringHash, internal::TransparentStringEqual> mSelectedStrings;
    bool mFastPathArmed = false;
    /// True iff every selected value resolved to an id at construction.
    bool mAllResolved = false;
    /// True iff the constructor was given an empty selection.
    /// `MatchesRow` then rejects every row. Named sentinel so future
    /// field additions don't accidentally break the inference.
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

/// Inclusive numeric-range predicate over `int64_t`, `uint64_t`, and
/// `double` slots. Either bound may be `std::nullopt` to leave that
/// side unbounded; non-numeric slots reject.
///
/// Compare type is `double` for unification. Above 2^53 the cast from
/// 64-bit integers loses precision, which is acceptable for the
/// range filter (callers wanting bit-exact integer boundaries can use
/// `TimeRangeRowPredicate`, which is int64-native).
/// `NaN` slots reject; a `NaN` bound is treated as unbounded.
class NumericRangeRowPredicate
{
public:
    NumericRangeRowPredicate(size_t columnIndex, std::optional<double> minValue, std::optional<double> maxValue);

    NumericRangeRowPredicate(const NumericRangeRowPredicate &) = default;
    NumericRangeRowPredicate &operator=(const NumericRangeRowPredicate &) = default;
    NumericRangeRowPredicate(NumericRangeRowPredicate &&) noexcept = default;
    NumericRangeRowPredicate &operator=(NumericRangeRowPredicate &&) noexcept = default;
    ~NumericRangeRowPredicate() = default;

    [[nodiscard]] bool MatchesRow(const LogTable &table, size_t row) const;

    /// Column index this predicate targets, in `LogTable` coords.
    [[nodiscard]] size_t ColumnIndex() const noexcept
    {
        return mColumnIndex;
    }

private:
    size_t mColumnIndex = 0;
    std::optional<double> mMin;
    std::optional<double> mMax;
};

/// Two-state predicate for `Type::Boolean` columns. `includeTrue` /
/// `includeFalse` carry the picked sides; if neither is set the
/// predicate rejects every row (mirrors the empty-`EnumRowPredicate`
/// behaviour). Non-bool slots also reject.
class BoolRowPredicate
{
public:
    BoolRowPredicate(size_t columnIndex, bool includeTrue, bool includeFalse);

    BoolRowPredicate(const BoolRowPredicate &) = default;
    BoolRowPredicate &operator=(const BoolRowPredicate &) = default;
    BoolRowPredicate(BoolRowPredicate &&) noexcept = default;
    BoolRowPredicate &operator=(BoolRowPredicate &&) noexcept = default;
    ~BoolRowPredicate() = default;

    [[nodiscard]] bool MatchesRow(const LogTable &table, size_t row) const;

    /// Column index this predicate targets, in `LogTable` coords.
    [[nodiscard]] size_t ColumnIndex() const noexcept
    {
        return mColumnIndex;
    }

    [[nodiscard]] bool IncludeTrue() const noexcept
    {
        return mIncludeTrue;
    }

    [[nodiscard]] bool IncludeFalse() const noexcept
    {
        return mIncludeFalse;
    }

private:
    size_t mColumnIndex = 0;
    bool mIncludeTrue = false;
    bool mIncludeFalse = false;
};

/// String predicate that defers matching to a caller-supplied
/// callback. Keeps Qt-flavoured regex / wildcard semantics in the GUI
/// without pulling Qt into the lib. The caller owns callback
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

/// Closed-set union of concrete row predicates. Stored by value, so
/// the per-row hot path pays no heap allocation and no virtual dispatch.
using RowPredicate = std::variant<
    EnumRowPredicate,
    TimeRangeRowPredicate,
    NumericRangeRowPredicate,
    BoolRowPredicate,
    CallbackStringRowPredicate>;

/// Visit-dispatch helper. Resolves to the concrete `MatchesRow` at
/// compile time.
[[nodiscard]] inline bool MatchesRow(const RowPredicate &predicate, const LogTable &table, size_t row)
{
    return std::visit([&table, row](const auto &concrete) { return concrete.MatchesRow(table, row); }, predicate);
}

/// Column index targeted by @p predicate. The GUI proxy uses this to
/// decide whether a source `dataChanged` requires a filter rebuild.
[[nodiscard]] inline size_t RowPredicateColumn(const RowPredicate &predicate)
{
    return std::visit([](const auto &concrete) noexcept { return concrete.ColumnIndex(); }, predicate);
}

/// Evaluate @p predicates against every row of @p table in parallel
/// and return the rows that pass all predicates, in ascending order.
/// Empty @p predicates returns `[0, table.RowCount())`.
///
/// Threading: each worker accumulates surviving rows into a
/// thread-local bucket via `tbb::parallel_for`; buckets are coalesced
/// and sorted on the caller thread before return. Predicate
/// `MatchesRow` implementations must be thread-safe read-only against
/// @p table -- every predicate in this file qualifies
/// (`CallbackStringRowPredicate` uses a `thread_local` buffer,
/// `EnumRowPredicate` reads an immutable bitset snapshot,
/// `TimeRangeRowPredicate` / `NumericRangeRowPredicate` /
/// `BoolRowPredicate` are stateless). Lives in `loglib` so callers
/// don't need a TBB include.
[[nodiscard]] std::vector<size_t> FilterAcceptedRows(const LogTable &table, std::span<const RowPredicate> predicates);

} // namespace loglib
