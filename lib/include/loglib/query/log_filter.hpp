/**
 * @brief Row predicates and the evaluator that applies a filter to a table.
 *
 * The predicates here test one row: enumeration membership, a time range, a number range, a boolean, or a string match
 * supplied as a callback. A compiled filter expression combines predicates with and, or and not. FilterAcceptedRows
 * applies it to every row of a table in parallel and returns the accepted rows.
 *
 * This is part of the query module.
 */
#pragma once

#include "loglib/core/enum_dictionary.hpp"
#include "loglib/core/transparent_string_hash.hpp"
#include "loglib/schema/filter_expression.hpp"

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

/**
 * @brief Accepts rows whose value in an enumeration column is one of several selected values.
 *
 * The fast path reads the cell's enumeration id and tests it in a bit set. A cell that is not encoded yet, because the
 * column has not become an enumeration, is compared as text against the selected values. The bit set is a snapshot of
 * the dictionary taken at construction, so build a new predicate when the dictionary changes. An old predicate still
 * gives correct answers: an id beyond the snapshot is rejected if every selected value was resolved, and otherwise is
 * compared as text.
 *
 * MatchesRow changes nothing after construction, so parallel threads may call it, even while a writer adds values to
 * the dictionary.
 */
class EnumRowPredicate
{
public:
    /**
     * @brief Builds the predicate for a column and the values to accept. The dictionary may be null if the column is
     * not an enumeration yet.
     */
    EnumRowPredicate(
        size_t columnIndex, std::span<const std::string_view> selectedValues, const EnumDictionary *dictionary
    );

    EnumRowPredicate(const EnumRowPredicate &) = delete;
    EnumRowPredicate &operator=(const EnumRowPredicate &) = delete;
    EnumRowPredicate(EnumRowPredicate &&) noexcept = default;
    EnumRowPredicate &operator=(EnumRowPredicate &&) noexcept = default;
    ~EnumRowPredicate() = default;

    [[nodiscard]] bool MatchesRow(const LogTable &table, size_t row) const;

    /** @brief The column this predicate tests, as a LogTable column index. */
    [[nodiscard]] size_t ColumnIndex() const noexcept
    {
        return mColumnIndex;
    }

    /** @brief True if at least one selected value was found in the dictionary, so the bit set test is used. */
    [[nodiscard]] bool IsFastPathArmed() const noexcept
    {
        return mFastPathArmed;
    }

private:
    size_t mColumnIndex = 0;

    /** @brief One bit per enumeration id, set for selected values. Empty when there was no dictionary. */
    std::vector<bool> mSelectedIds;

    /**
     * @brief The selected values that were not found in the dictionary, or all of them without a dictionary. Compared
     * as text.
     */
    std::unordered_set<std::string, loglib::TransparentStringHash, loglib::TransparentStringEqual> mSelectedStrings;
    bool mFastPathArmed = false;

    /** @brief True if every selected value was found in the dictionary at construction. */
    bool mAllResolved = false;

    /** @brief True if no value was selected, so every row is rejected. */
    bool mEmptySelection = false;
};

/**
 * @brief Accepts rows whose time lies in a range. Both ends are included and are microseconds since the epoch. A range
 * whose begin is after its end accepts nothing.
 */
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

    /** @brief The column this predicate tests, as a LogTable column index. */
    [[nodiscard]] size_t ColumnIndex() const noexcept
    {
        return mColumnIndex;
    }

private:
    size_t mColumnIndex = 0;
    int64_t mBegin = 0;
    int64_t mEnd = 0;
};

/**
 * @brief Accepts rows whose number lies in a range. Both ends are included.
 *
 * Cells that are not numbers are rejected. A missing bound means no limit. Values are compared as doubles, so integers
 * beyond 2^53 lose precision; use TimeRangeRowPredicate when exact 64-bit integers matter. A NaN cell is rejected, a
 * NaN bound counts as no limit, and an infinite bound is honored.
 */
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

    /** @brief The column this predicate tests, as a LogTable column index. */
    [[nodiscard]] size_t ColumnIndex() const noexcept
    {
        return mColumnIndex;
    }

private:
    size_t mColumnIndex = 0;
    std::optional<double> mMin;
    std::optional<double> mMax;
};

/**
 * @brief Accepts rows whose boolean is true, false, or either, as selected. With neither selected every row is
 * rejected, like an enumeration predicate with no values. Cells that are not booleans are rejected.
 */
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

    /** @brief The column this predicate tests, as a LogTable column index. */
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

/**
 * @brief Accepts rows whose text satisfies a function the caller supplies.
 *
 * MakeStringMatcher builds the functions for regular expressions and wildcards. Filtering calls the function from
 * several threads, so it must be safe to copy and to call concurrently.
 */
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

    /** @brief The column this predicate tests, as a LogTable column index. */
    [[nodiscard]] size_t ColumnIndex() const noexcept
    {
        return mColumnIndex;
    }

private:
    size_t mColumnIndex = 0;
    MatchFn mMatch;
};

/**
 * @brief Any one of the row predicates. It is stored by value, so testing a row needs no allocation and no virtual
 * call.
 */
using RowPredicate = std::variant<
    EnumRowPredicate,
    TimeRangeRowPredicate,
    NumericRangeRowPredicate,
    BoolRowPredicate,
    CallbackStringRowPredicate>;

/** @brief Tests a row with whichever predicate the variant holds. */
[[nodiscard]] inline bool MatchesRow(const RowPredicate &predicate, const LogTable &table, size_t row)
{
    return std::visit([&table, row](const auto &concrete) { return concrete.MatchesRow(table, row); }, predicate);
}

/** @brief The column a predicate tests, as a LogTable column index. */
[[nodiscard]] inline size_t RowPredicateColumn(const RowPredicate &predicate)
{
    return std::visit([](const auto &concrete) noexcept { return concrete.ColumnIndex(); }, predicate);
}

/**
 * @brief A relative cost for testing one row with this predicate, used to try cheap tests first.
 *
 * The numbers have no unit and only their order matters: Boolean 1, Enumeration 2, Time 3, Numeric 4 and String 10.
 */
[[nodiscard]] int EstimatedLeafCost(const RowPredicate &predicate) noexcept;

/**
 * @brief A filter expression with its predicates already built, ready to test rows.
 *
 * It mirrors FilterExpression. Leaves hold a row predicate, and And, Or and Not nodes own their children. Each node
 * stores its estimated cost, and CompileExpression orders the children of And and Or cheapest first. It can be moved
 * but not copied. Evaluating it changes nothing, so FilterAcceptedRows may use it from many threads.
 */
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

    /** @brief The root of the expression. The default is an empty And, which matches every row. */
    Node node = And{};

    /**
     * @brief The columns the leaves test, sorted and without duplicates. A view can use them to tell whether a change
     * to a column affects the filter.
     */
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

/**
 * @brief Tests one row against an expression.
 *
 * An empty And accepts, an empty Or rejects, and Not inverts. And and Or stop at the first child that decides the
 * result, and the children are already ordered cheapest first.
 */
[[nodiscard]] bool EvaluateExpression(const CompiledFilterExpression &expression, const LogTable &table, size_t row);

/** @brief True if the expression is an empty And, which accepts every row. */
[[nodiscard]] bool IsMatchAllCompiled(const CompiledFilterExpression &expression) noexcept;

/**
 * @brief Tests every row of the table in parallel and returns the accepted rows in ascending order.
 *
 * It uses one of two methods for each call:
 *
 * - Row by row, the default: worker threads call EvaluateExpression for their share of the rows.
 * - Bit sets, for expressions with an Or or a Not and at least two leaves, or at least four leaves in all, as long as
 *   the bit sets need at most 512 MiB. Each distinct leaf is evaluated once into a packed bit set, and the tree is
 *   then combined with word-wide AND, OR and NOT. A flat And never uses it, because stopping early is cheaper.
 *
 * Each worker collects its own results and the calls combine and sort them. Every predicate is only read.
 */
[[nodiscard]] std::vector<size_t> FilterAcceptedRows(const LogTable &table, const CompiledFilterExpression &expression);

} // namespace loglib
