/**
 * @brief Filter rules and the boolean tree that combines them.
 *
 * A LeafRule matches one column. A FilterExpression combines leaves with And, Or and Not. Leaves refer to columns by
 * key, not by position, so a rule keeps working when columns are moved or added. Compiling an expression into something
 * fast to run is the job of the query module.
 *
 * This is part of the schema module.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace loglib
{

/**
 * @brief One condition of a filter, on a single column.
 *
 * A leaf finds its column by columnKeys, which are matched against Column::keys, so it keeps working when columns are
 * moved or added. HighlightRule finds its column the same way. A leaf without keys does nothing. Only the filter fields
 * that belong to the leaf's type are used; the others stay at their defaults.
 */
struct LeafRule
{
    /**
     * @brief The kind of condition. It decides which row predicate is built from the leaf.
     *
     * The saved files store these values, so add new kinds at the end and never reorder them.
     */
    enum class Type
    {
        String,
        Time,
        /**
         * @brief Several selected values of an enumeration column, saved as strings and turned into a bit set when the
         * leaf is compiled.
         */
        Enumeration,
        /**
         * @brief A range of numbers for an Integer, Floating or Number column. It uses filterMinValue and
         * filterMaxValue, and either may be missing for no limit.
         */
        Number,
        /**
         * @brief Selects true, false or both. filterValues holds a subset of "true" and "false", and an empty set
         * rejects every row.
         */
        Boolean
    };

    /**
     * @brief How a String leaf compares text. The query language writes ~ for RegularExpression, = for Exactly, : for
     * Contains and % for Wildcard.
     */
    enum class Match
    {
        Exactly,
        Contains,
        RegularExpression,
        Wildcard
    };

    Type type = Type::String;

    /**
     * @brief The keys of the column to test, matched as a subset of Column::keys. Usually one key. Several keys select
     * a column that is a combination of keys.
     */
    std::vector<std::string> columnKeys;

    /** @brief How to compare text. Needed for a String leaf. */
    std::optional<Match> matchType;

    /** @brief The text to look for. Needed for a String leaf. */
    std::optional<std::string> filterString;

    /** @brief The bounds of a Time leaf, as microseconds since the epoch, both included. Set at least one. */
    std::optional<int64_t> filterBegin;
    std::optional<int64_t> filterEnd;

    /** @brief The bounds of a Number leaf, both included. A missing bound means no limit. */
    std::optional<double> filterMinValue;
    std::optional<double> filterMaxValue;

    /** @brief The selected values of an Enumeration or Boolean leaf. */
    std::vector<std::string> filterValues;

    friend bool operator==(const LeafRule &, const LeafRule &) = default;
};

/**
 * @brief A tree of conditions that says which rows the user wants to see.
 *
 * A node is one of:
 * - Leaf: one LeafRule.
 * - And: accepts a row when every child does. An empty And accepts every row, and it is the default.
 * - Or: accepts a row when any child does. An empty Or accepts no row.
 * - Not: accepts a row when its child rejects it.
 *
 * The child of a Not is held by pointer so the type can contain itself and still be moved.
 */
// NOLINTBEGIN(misc-non-private-member-variables-in-classes,misc-no-recursion)
struct FilterExpression
{
    struct Leaf
    {
        LeafRule rule;

        friend bool operator==(const Leaf &, const Leaf &) = default;
    };

    struct And
    {
        std::vector<FilterExpression> children;

        friend bool operator==(const And &, const And &) = default;
    };

    struct Or
    {
        std::vector<FilterExpression> children;

        friend bool operator==(const Or &, const Or &) = default;
    };

    /**
     * @brief Inverts one child. The child is set once the node is built. A null child, which only tests can create,
     * counts as match-all.
     */
    struct Not
    {
        std::unique_ptr<FilterExpression> child;

        Not() = default;
        explicit Not(FilterExpression c)
            : child(std::make_unique<FilterExpression>(std::move(c)))
        {
        }
        Not(const Not &other)
            : child(other.child ? std::make_unique<FilterExpression>(*other.child) : nullptr)
        {
        }
        Not &operator=(const Not &other)
        {
            if (this != &other)
            {
                child = other.child ? std::make_unique<FilterExpression>(*other.child) : nullptr;
            }
            return *this;
        }
        Not(Not &&) noexcept = default;
        Not &operator=(Not &&) noexcept = default;
        ~Not() = default;

        friend bool operator==(const Not &lhs, const Not &rhs)
        {
            if (lhs.child == nullptr && rhs.child == nullptr)
            {
                return true;
            }
            if (lhs.child == nullptr || rhs.child == nullptr)
            {
                return false;
            }
            return *lhs.child == *rhs.child;
        }
    };

    /** @brief The node. The default is an empty And, which accepts every row. */
    std::variant<Leaf, And, Or, Not> node = And{};

    friend bool operator==(const FilterExpression &, const FilterExpression &) = default;
};
// NOLINTEND(misc-non-private-member-variables-in-classes,misc-no-recursion)

/** @brief True if the expression is an empty And, which accepts every row. */
[[nodiscard]] inline bool IsMatchAll(const FilterExpression &expr) noexcept
{
    const auto *asAnd = std::get_if<FilterExpression::And>(&expr.node);
    return asAnd != nullptr && asAnd->children.empty();
}

[[nodiscard]] inline FilterExpression MakeLeaf(LeafRule rule)
{
    FilterExpression expr;
    expr.node = FilterExpression::Leaf{std::move(rule)};
    return expr;
}

[[nodiscard]] inline FilterExpression MakeAnd(std::vector<FilterExpression> children)
{
    FilterExpression expr;
    expr.node = FilterExpression::And{std::move(children)};
    return expr;
}

[[nodiscard]] inline FilterExpression MakeOr(std::vector<FilterExpression> children)
{
    FilterExpression expr;
    expr.node = FilterExpression::Or{std::move(children)};
    return expr;
}

[[nodiscard]] inline FilterExpression MakeNot(FilterExpression child)
{
    FilterExpression expr;
    expr.node = FilterExpression::Not{std::move(child)};
    return expr;
}

} // namespace loglib
