/**
 * @brief Parses the text of the advanced filter into a filter expression.
 *
 * ParseQuery reads a query with and, or, not and groups, and returns either a FilterExpression or an error with the
 * position of the problem. FormatExpression writes an expression back as text. Nesting depth is limited.
 *
 * This is part of the query module.
 */
#pragma once

#include "loglib/schema/filter_expression.hpp"
#include "loglib/time/time_zone_context.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace loglib
{

/** @brief Deepest group and NOT nesting ParseQuery accepts. */
inline constexpr std::size_t MAX_QUERY_NESTING = 64;

/**
 * @brief A syntax error from ParseQuery: the byte position in the input, which is the size of the input at its end,
 * and a one-line message.
 */
struct QueryParseError
{
    std::size_t offset = 0;
    std::string message;
};

/**
 * @brief Parses query text into a FilterExpression.
 *
 * Grammar:
 *
 *   query    := or_expr
 *   or_expr  := and_expr ( ('OR'|'or'|'||') and_expr )*
 *   and_expr := not_expr ( ('AND'|'and'|'&&') not_expr )*
 *               -- adjacent not_exprs compose as implicit AND
 *   not_expr := ('NOT'|'not'|'!') not_expr | atom
 *   atom     := '(' or_expr ')' | leaf
 *
 *   leaf     := column op value
 *             | column ('IN'|'in') value_list
 *
 *   column   := ident | quoted-string
 *   op       := ':' | '=' | '~' | '%' | '>' | '>=' | '<' | '<='
 *
 *   value      := quoted-string | number | 'true' | 'false' | bareword | regex-lit
 *   regex-lit  := '/' ... '/'    (only valid after '~')
 *   value_list := '[' value (',' value)* ']'
 *               | '[' bound? '..' bound? ']'     (range form)
 *
 * Leaf semantics:
 *   col:val         String Contains
 *   col="val"       String Exactly
 *   col~/re/        String RegularExpression
 *   col%"pat"       String Wildcard
 *   col=N           Numeric equal
 *   col>N / >=N / <N / <=N   Numeric one-sided range
 *   col=true / =false        Boolean
 *   col IN [a,b,c]  Enumeration multi-select
 *   col IN [a..b]   Numeric or Time range (Time when bounds
 *                   parse as ISO timestamps, else Numeric;
 *                   either bound may be empty)
 *
 * An ISO timestamp without Z or an offset is read in timeZone.
 *
 * AND, OR, NOT and IN can be written in any case, and FormatExpression writes them in upper case. The column name is
 * stored as written, as the only element of LeafRule::columnKeys. Empty text, or text with only whitespace, gives a
 * match-all expression.
 */
[[nodiscard]] std::expected<FilterExpression, QueryParseError> ParseQuery(
    std::string_view input, const TimeZoneContext &timeZone
);

/**
 * @brief Writes an expression as query text. ParseQuery reads back the same expression, for every tree it can produce.
 *
 * - A bareword is put in quotes when it contains whitespace or operator characters, or starts with a digit. A regular
 *   expression is always written between slashes.
 * - An empty And at the top gives empty text. An empty And deeper in the tree, which the parser never makes, is written
 *   as * for debugging, and an empty Or as ().
 * - An Or under an And or a Not gets parentheses. A Not gets parentheses around its child unless the child is a leaf or
 *   another Not.
 */
[[nodiscard]] std::string FormatExpression(const FilterExpression &expression);

} // namespace loglib
