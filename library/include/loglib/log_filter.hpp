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
///
/// Hot path: one `GetEnumValueId` + `vector<bool>` test. Rows whose
/// slot isn't a `DictRef` (column not promoted yet) fall back to a
/// transparent-hash string set. The bitset is a dictionary snapshot
/// from construction time; rebuild on `enumColumnsChanged` for
/// freshness. Stale predicates stay correct: ids past the bitset
/// end reject when `mAllResolved`, else use the string set.
///
/// Threading: `MatchesRow` is stateless post-construction and safe
/// from `tbb::parallel_for`, even if a writer grows the dictionary
/// mid-evaluation (new ids go past the bitset; string set handles them).
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
    /// Selected values that didn't resolve at construction (or all
    /// of them if no dictionary was given). Covers unpromoted slots
    /// and past-bitset hits from stale predicates.
    std::unordered_set<std::string, internal::TransparentStringHash, internal::TransparentStringEqual> mSelectedStrings;
    bool mFastPathArmed = false;
    /// Every selected value resolved to an id at construction time.
    bool mAllResolved = false;
    /// Empty selection sentinel; `MatchesRow` rejects every row.
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

/// Inclusive numeric range over `int64_t` / `uint64_t` / `double`
/// slots. Non-numeric slots reject; `nullopt` on a side means
/// unbounded.
///
/// Compares as `double`, so integer precision loss past `2^53` is
/// accepted (use `TimeRangeRowPredicate` for bit-exact int64).
/// `NaN` slots reject; `NaN` bounds collapse to unbounded; `±inf`
/// bounds are honoured. The GUI rejects `NaN`/`±inf` upstream.
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

/// Two-state predicate for `Type::Boolean`. Neither side selected
/// = reject every row (like empty `EnumRowPredicate`). Non-bool
/// slots reject too.
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

/// String predicate that defers to a caller-supplied callback.
/// Keeps Qt-flavoured regex/wildcard semantics in the GUI without
/// pulling Qt into the lib. The caller owns callback thread-safety;
/// the GUI builder pre-JITs its `QRegularExpression`.
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

/// Closed union of concrete row predicates. Stored by value; the
/// per-row hot path pays no heap allocation or virtual dispatch.
/// Alternative order is stable on disk -- append only.
using RowPredicate = std::variant<
    EnumRowPredicate,
    TimeRangeRowPredicate,
    NumericRangeRowPredicate,
    BoolRowPredicate,
    CallbackStringRowPredicate>;

/// Visit-dispatch helpers; compile-time-resolved to the concrete leaf.
[[nodiscard]] inline bool MatchesRow(const RowPredicate &predicate, const LogTable &table, size_t row)
{
    return std::visit([&table, row](const auto &concrete) { return concrete.MatchesRow(table, row); }, predicate);
}

[[nodiscard]] inline size_t RowPredicateColumn(const RowPredicate &predicate)
{
    return std::visit([](const auto &concrete) noexcept { return concrete.ColumnIndex(); }, predicate);
}

/// Relative cost weight for cheap-first child ordering in
/// `And`/`Or`. No unit; only the monotonic order matters.
///   Bool     - 1  (alt-check on decoded slot)
///   Enum     - 2  (id lookup + bitset test)
///   Time     - 3  (int64 compare)
///   Numeric  - 4  (double compare with type coercion)
///   String   - 10 (regex / UTF-8 walk / callback)
[[nodiscard]] int EstimatedLeafCost(const RowPredicate &predicate) noexcept;

/// Compiled mirror of `FilterExpression`. Leaves hold pre-built
/// `RowPredicate`s; combinators own their children; each node
/// caches `estimatedCost` so children stay cheap-first from
/// `CompileExpression`.
///
/// Move-only in practice (`EnumRowPredicate` is move-only, so
/// callers move rather than copy). Evaluation is read-only, safe
/// from `tbb::parallel_for` in `FilterAcceptedRows`.
///
/// clang-tidy suppression: `misc-non-private-member-variables-in-classes`
/// -- the alternatives are transparent data holders that the
/// evaluator, compiler, and tests read / write directly by field
/// name. Same treatment as `FilterExpression`.
// NOLINTBEGIN(misc-non-private-member-variables-in-classes)
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

    /// Default = empty `And` (match-all).
    Node node = And{};

    /// Column indices referenced by leaves, sorted + deduped.
    /// `LogFilterModel` uses this to decide whether a source
    /// `dataChanged` requires a full rebuild.
    std::vector<size_t> referencedColumns;

    [[nodiscard]] int EstimatedCost() const noexcept;

    CompiledFilterExpression() = default;
    CompiledFilterExpression(const CompiledFilterExpression &) = delete;
    CompiledFilterExpression &operator=(const CompiledFilterExpression &) = delete;
    CompiledFilterExpression(CompiledFilterExpression &&) noexcept = default;
    CompiledFilterExpression &operator=(CompiledFilterExpression &&) noexcept = default;
    ~CompiledFilterExpression() = default;
};
// NOLINTEND(misc-non-private-member-variables-in-classes)

/// Short-circuiting per-row evaluator. Empty `And` = true, empty
/// `Or` = false, `Not` inverts, `And`/`Or` stop at the first
/// decisive child. Cheap-first ordering is baked in by
/// `CompileExpression`.
[[nodiscard]] bool EvaluateExpression(const CompiledFilterExpression &expression, const LogTable &table, size_t row);

/// True iff @p expression is an empty `And` (match-all).
[[nodiscard]] bool IsMatchAllCompiled(const CompiledFilterExpression &expression) noexcept;

/// Evaluate @p expression across every row of @p table in parallel
/// and return the accepted rows in ascending order.
///
/// Picks one of two paths per rebuild:
///
/// - **Visit path** (default): `tbb::parallel_for` over rows, each
///   row calling `EvaluateExpression`. Same envelope as the old
///   flat `span<RowPredicate>` for flat `And` trees.
/// - **Bitset materialisation path**: kicks in for trees with an
///   `OR`/`NOT` and >=2 total leaves (or >=4 leaves overall), when
///   `row_count * unique_leaves / 8 <= 512 MiB`. Flat `And` never
///   qualifies (short-circuiting beats materialising). Each unique
///   leaf's accept-set becomes a packed bitset (shared across
///   repeats); the tree walks with word-parallel AND/OR/NOT.
///
/// Threading: per-worker thread-local buckets/bitsets; the caller
/// coalesces and sorts. Every predicate is read-only-safe.
[[nodiscard]] std::vector<size_t> FilterAcceptedRows(const LogTable &table, const CompiledFilterExpression &expression);

} // namespace loglib
