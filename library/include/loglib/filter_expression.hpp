#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace loglib
{

/// One leaf of a `FilterExpression`: a single-column match spec.
///
/// Bound to its column by `columnKeys` (subset-matched against
/// `Column::keys`), so leaves survive `MoveColumn`, cross-source
/// apply, and column additions without any remap step. Empty
/// `columnKeys` -> inert at compile time. Mirrors the binding
/// used by `LogConfiguration::HighlightRule`.
///
/// Only the `filter*` fields relevant to the selected `type` are
/// read at compile time; the rest stay defaulted.
struct LeafRule
{
    /// Match kind; picks the concrete `RowPredicate` at compile
    /// time. On-disk values are stable -- append, never reorder.
    enum class Type
    {
        String,
        Time,
        /// Multi-select over an enum column. Persisted as
        /// strings; resolved to a bitset at rule construction.
        Enumeration,
        /// Inclusive numeric range for `Integer`/`Floating`/`Number`.
        /// Carried in `filterMinValue`/`filterMaxValue`; either
        /// side may be `nullopt` for unbounded.
        Number,
        /// True/false multi-select. `filterValues` is a subset of
        /// `{"true","false"}`; empty rejects every row.
        Boolean
    };

    /// String matching flavour. Used by `Type::String` only; the
    /// parser routes `~` -> `RegularExpression`, `=` -> `Exactly`,
    /// `:` -> `Contains`, `%` -> `Wildcard`.
    enum class Match
    {
        Exactly,
        Contains,
        RegularExpression,
        Wildcard
    };

    Type type = Type::String;

    /// Column identity, subset-matched against `Column::keys`.
    /// Empty = inert. Usually one entry; multi-key leaves cover
    /// synthetic columns whose identity is a combination of keys.
    std::vector<std::string> columnKeys;

    /// String-match flavour. Required for `Type::String`.
    std::optional<Match> matchType;

    /// String needle. Required for `Type::String`.
    std::optional<std::string> filterString;

    /// Inclusive time-range bounds in microseconds since epoch.
    /// At least one side must be set to be meaningful.
    std::optional<int64_t> filterBegin;
    std::optional<int64_t> filterEnd;

    /// Inclusive numeric range bounds for `Type::Number`.
    /// `nullopt` means -inf / +inf.
    std::optional<double> filterMinValue;
    std::optional<double> filterMaxValue;

    /// Selected values for `Type::Enumeration` and `Type::Boolean`.
    std::vector<std::string> filterValues;

    friend bool operator==(const LeafRule &, const LeafRule &) = default;
};

/// Boolean expression tree over `LeafRule` leaves. Canonical
/// representation of "which rows the user wants to see".
///
/// Wire format:
/// - `Leaf`: one `LeafRule`.
/// - `And`: matches when every child matches. Empty `And` = match-all
///   (identity element; the default-constructed expression).
/// - `Or`: matches when any child matches. Empty `Or` = match-none.
/// - `Not`: matches when the wrapped child rejects.
///
/// `Not::child` is a `unique_ptr` so the type is complete at
/// instantiation time and stays movable.
///
/// clang-tidy suppressions:
///   * `misc-non-private-member-variables-in-classes` -- the four
///     alternatives are transparent data holders; Glaze meta and
///     every consumer reads / writes fields by name.
///   * `misc-no-recursion` -- `FilterExpression` recursively contains
///     `And{children}`, `Or{children}`, `Not{child}`, so every
///     defaulted `operator==` and the `Not` copy ctor/assign chain
///     through themselves. That is the desired behaviour; the check
///     has no per-file gate we can use to distinguish "structural
///     recursion" from "unbounded stack recursion".
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

    /// Wraps a single child. Non-null once fully constructed; a
    /// null child (only reachable via direct field mutation in
    /// tests) is treated as an empty `NOT` = match-all.
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

    /// Default = empty `And` (match-all).
    std::variant<Leaf, And, Or, Not> node = And{};

    friend bool operator==(const FilterExpression &, const FilterExpression &) = default;
};
// NOLINTEND(misc-non-private-member-variables-in-classes,misc-no-recursion)

/// True iff @p expr is an empty `And` (matches every row).
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
