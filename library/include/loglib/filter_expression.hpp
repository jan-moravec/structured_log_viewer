#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace loglib
{

/// One leaf of a `FilterExpression` -- a single-column match spec.
///
/// Bound to its column by `columnKeys` (subset-matched against
/// `LogConfiguration::Column::keys`). Empty `columnKeys` = no
/// column bound (leaf is inert at compile time). Mirrors the
/// key-based binding used by `LogConfiguration::HighlightRule`,
/// so leaves survive `MoveColumn`, cross-source apply, and
/// column additions without any remap step.
///
/// The `type` / `matchType` / `filter*` fields carry the payload
/// needed by the concrete `RowPredicate` chosen at compile time.
/// Only the fields relevant to the selected `type` are consulted;
/// the rest stay defaulted. `LeafRule::Match` mirrors the shape of
/// `QRegularExpression` / `QRegularExpression::wildcardToRegularExpression`
/// so the app-side `MakeStringMatcher` factory works uniformly.
struct LeafRule
{
    /// Match kind. Selects the concrete `RowPredicate` at compile
    /// time. Values are stable on disk (see the Glaze meta) --
    /// append at the end, never reorder existing entries.
    enum class Type
    {
        String,
        Time,
        /// Multi-select over an enum column. Persisted as strings,
        /// resolved to a bitset at rule construction.
        Enumeration,
        /// Inclusive numeric range for `Integer` / `Floating` /
        /// `Number` columns. Carried in `filterMinValue` /
        /// `filterMaxValue`; either may be `nullopt` for unbounded.
        Number,
        /// True / false multi-select for `Type::Boolean` columns.
        /// `filterValues` is a subset of `{"true", "false"}`;
        /// empty rejects every row.
        Boolean
    };

    /// String matching flavour. Only meaningful for
    /// `Type::String`; the parser also uses it to route `~` to
    /// `RegularExpression`, `=` to `Exactly`, and `:` to
    /// `Contains`.
    enum class Match
    {
        Exactly,
        Contains,
        RegularExpression,
        Wildcard
    };

    Type type = Type::String;

    /// Column identity (subset-matched against `Column::keys`).
    /// Empty = inert. Usually a single-entry vector; multi-key
    /// leaves cover synthetic columns whose stable identity is a
    /// combination of raw keys.
    std::vector<std::string> columnKeys;

    /// String-match flavour. Required for `Type::String`,
    /// ignored otherwise.
    std::optional<Match> matchType;

    /// String needle. Required for `Type::String`, ignored otherwise.
    std::optional<std::string> filterString;

    /// Inclusive time-range bounds in microseconds since epoch.
    /// `nullopt` on either side means unbounded on that side; at
    /// least one bound must be set to be meaningful.
    std::optional<int64_t> filterBegin;
    std::optional<int64_t> filterEnd;

    /// Inclusive numeric range bounds for `Type::Number`. `nullopt`
    /// means unbounded (-inf / +inf).
    std::optional<double> filterMinValue;
    std::optional<double> filterMaxValue;

    /// Selected values for `Type::Enumeration` and `Type::Boolean`.
    /// Empty otherwise.
    std::vector<std::string> filterValues;

    friend bool operator==(const LeafRule &, const LeafRule &) = default;
};

/// A boolean expression tree over `LeafRule` leaves. The single
/// canonical representation of "which rows the user wants to see".
///
/// Wire format:
/// - `Leaf` -- one `LeafRule`.
/// - `And` -- zero or more child expressions; matches when every
///   child matches. Default-constructed `FilterExpression` is an
///   empty `And`, which matches every row (identity element).
/// - `Or` -- zero or more child expressions; matches when any child
///   matches. Empty `Or` matches nothing (identity element).
/// - `Not` -- wraps a single child; matches when the child rejects.
///
/// `And::children` and `Or::children` hold `FilterExpression` by
/// value; the enclosing variant + `std::vector<FilterExpression>`
/// pattern is portable across libstdc++, libc++, and MSVC STL under
/// C++23. `Not::child` uses `std::unique_ptr` to guarantee
/// completeness at instantiation time and keep the type moveable.
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

    /// Wraps a single child expression. The pointer is never null
    /// once the `Not` is fully constructed (constructors below
    /// enforce it); test-only code that mutates the field
    /// directly should treat null as "empty NOT = matches every
    /// row" (double-negation of the empty `Or`).
    struct Not
    {
        std::unique_ptr<FilterExpression> child;

        Not() = default;
        explicit Not(FilterExpression c) : child(std::make_unique<FilterExpression>(std::move(c)))
        {
        }
        Not(const Not &other) : child(other.child ? std::make_unique<FilterExpression>(*other.child) : nullptr)
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

    /// Default-constructed expression is an empty `And` (matches every row).
    std::variant<Leaf, And, Or, Not> node = And{};

    friend bool operator==(const FilterExpression &, const FilterExpression &) = default;
};

/// True iff @p expr is an empty `And` (default-constructed). Matches every row.
[[nodiscard]] inline bool IsMatchAll(const FilterExpression &expr) noexcept
{
    const auto *asAnd = std::get_if<FilterExpression::And>(&expr.node);
    return asAnd != nullptr && asAnd->children.empty();
}

/// Convenience: wrap a single leaf as an expression.
[[nodiscard]] inline FilterExpression MakeLeaf(LeafRule rule)
{
    FilterExpression expr;
    expr.node = FilterExpression::Leaf{std::move(rule)};
    return expr;
}

/// Convenience: wrap children as an `And` node.
[[nodiscard]] inline FilterExpression MakeAnd(std::vector<FilterExpression> children)
{
    FilterExpression expr;
    expr.node = FilterExpression::And{std::move(children)};
    return expr;
}

/// Convenience: wrap children as an `Or` node.
[[nodiscard]] inline FilterExpression MakeOr(std::vector<FilterExpression> children)
{
    FilterExpression expr;
    expr.node = FilterExpression::Or{std::move(children)};
    return expr;
}

/// Convenience: wrap a child as a `Not` node.
[[nodiscard]] inline FilterExpression MakeNot(FilterExpression child)
{
    FilterExpression expr;
    expr.node = FilterExpression::Not{std::move(child)};
    return expr;
}

} // namespace loglib
