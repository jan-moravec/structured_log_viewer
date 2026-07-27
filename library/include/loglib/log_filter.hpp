#pragma once

#include "loglib/enum_dictionary.hpp"
#include "loglib/filter_expression.hpp"
#include "loglib/internal/transparent_string_hash.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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

/// Inclusive numeric range over `int64_t`, `uint64_t`, and `double`
/// slots; non-numeric slots reject. `std::nullopt` leaves the
/// corresponding side unbounded.
///
/// Compares as `double` -- past `2^53` the cast from 64-bit integers
/// loses precision (acceptable for filter UX; use
/// `TimeRangeRowPredicate` for bit-exact int64). `NaN` slots reject;
/// `NaN` bounds collapse to unbounded; `±inf` bounds are honoured
/// literally. The GUI rejects `NaN`/`±inf` user input upstream.
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

/// Two-state predicate for `Type::Boolean` columns. With neither
/// side selected the predicate rejects every row (mirrors empty
/// `EnumRowPredicate`). Non-bool slots reject too.
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

/// Closed union of concrete row predicates. Stored by value -- the
/// per-row hot path pays no heap allocation or virtual dispatch.
///
/// Alternative order is stable: tests (and any future on-disk
/// serialiser) persist `variant::index()`. Only append new
/// alternatives; never insert.
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

/// Estimated relative cost of evaluating a single leaf predicate.
/// Used to order children in `And` / `Or` nodes so short-circuit
/// evaluation fires the cheapest rejecting / accepting leaf first.
/// Values are relative (there is no unit -- just monotonic ordering
/// vs. observed benchmark cost); tune per benchmark numbers.
///
///   Bool          -- 1  (single alt-check on a decoded slot)
///   Enum          -- 2  (id lookup + bitset test)
///   Time          -- 3  (int64 compare)
///   Numeric       -- 4  (double compare with type coercion)
///   String        -- 10 (regex / UTF-8 walk / callback)
[[nodiscard]] int EstimatedLeafCost(const RowPredicate &predicate) noexcept;

/// Compiled expression tree -- the "resolved" mirror of
/// `FilterExpression`. Leaves hold pre-built `RowPredicate`s;
/// `And` / `Or` / `Not` combinators own their children. Each node
/// carries a cached `estimatedCost` used to order children
/// cheap-first at compile time.
///
/// Trees are movable and copyable (the copy walks the tree and
/// clones each `RowPredicate`; the underlying predicate types are
/// copyable except `EnumRowPredicate`, which is move-only, so
/// callers currently favour move over copy). Threading:
/// evaluation is read-only, safe under `tbb::parallel_for` in
/// `FilterAcceptedRows`.
struct CompiledFilterExpression
{
    struct Leaf
    {
        RowPredicate predicate;
        int estimatedCost = 0;

        Leaf() = delete;
        explicit Leaf(RowPredicate p);
    };

    struct And
    {
        std::vector<CompiledFilterExpression> children;
        int estimatedCost = 0;
    };

    struct Or
    {
        std::vector<CompiledFilterExpression> children;
        int estimatedCost = 0;
    };

    struct Not
    {
        std::unique_ptr<CompiledFilterExpression> child;
        int estimatedCost = 0;

        Not() = default;
        explicit Not(CompiledFilterExpression c);
        Not(const Not &) = delete;
        Not &operator=(const Not &) = delete;
        Not(Not &&) noexcept = default;
        Not &operator=(Not &&) noexcept = default;
        ~Not() = default;
    };

    using Node = std::variant<Leaf, And, Or, Not>;

    /// Default-constructed = empty `And` (match every row).
    Node node = And{};

    /// Column indices referenced by any leaf in the tree, in
    /// ascending order and deduplicated. Used by `LogFilterModel`
    /// to decide whether a source `dataChanged` requires a full
    /// rebuild.
    std::vector<size_t> referencedColumns;

    [[nodiscard]] int EstimatedCost() const noexcept;

    CompiledFilterExpression() = default;
    CompiledFilterExpression(const CompiledFilterExpression &) = delete;
    CompiledFilterExpression &operator=(const CompiledFilterExpression &) = delete;
    CompiledFilterExpression(CompiledFilterExpression &&) noexcept = default;
    CompiledFilterExpression &operator=(CompiledFilterExpression &&) noexcept = default;
    ~CompiledFilterExpression() = default;
};

/// Evaluate @p expression against @p table row @p row. Uses the
/// short-circuiting visit path: an empty `And` returns `true`, an
/// empty `Or` returns `false`, `Not` inverts, `And` / `Or` stop at
/// the first decisive child. The per-node cheap-first ordering is
/// baked in at compile time (`CompileExpression` sorts children).
[[nodiscard]] bool EvaluateExpression(const CompiledFilterExpression &expression, const LogTable &table, size_t row);

/// True iff @p expression's tree is an empty `And` node -- i.e.
/// matches every row.
[[nodiscard]] bool IsMatchAllCompiled(const CompiledFilterExpression &expression) noexcept;

/// Evaluate @p expression against every row of @p table in parallel
/// and return the surviving rows in ascending order.
///
/// Chooses between two evaluators per rebuild:
///
/// - **Visit path** -- the default. `tbb::parallel_for` over rows,
///   each row calling `EvaluateExpression`. Same shape as the
///   previous `span<RowPredicate>` overload; identical performance
///   envelope for flat `And` trees.
/// - **Bitset materialisation path** -- kicks in when the tree is
///   complex (has OR / NOT / regex leaves and >=2 unique leaves,
///   or >=4 unique leaves overall) and the memory budget allows
///   (`row_count * unique_leaves / 8 <= 512 MiB`). Materialises
///   each unique leaf's accept-set into a packed bitset once,
///   then walks the tree with word-parallel AND / OR / NOT ops.
///   Wins on OR-heavy queries and regex leaves reused across
///   branches. Memory allocation is proportional to
///   `row_count * unique_leaves`, so the heuristic caps it.
///
/// Threading: per-worker thread-local buckets / bitsets; the caller
/// thread coalesces + sorts. `EvaluateExpression` must be
/// thread-safe read-only against @p table; every predicate qualifies.
[[nodiscard]] std::vector<size_t> FilterAcceptedRows(
    const LogTable &table, const CompiledFilterExpression &expression
);

} // namespace loglib
