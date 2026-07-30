#pragma once

#include "loglib/filter_expression.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace loglib
{

/// Parse error surfaced by `ParseQuery`: a byte offset into the
/// input (clamped to `input.size()` at EOI) and a one-line message.
struct QueryParseError
{
    std::size_t offset = 0;
    std::string message;
};

/// Parse @p input into a `FilterExpression`.
///
/// Grammar (v1):
///
///   query    := or_expr
///   or_expr  := and_expr ( ('OR'|'or'|'||') and_expr )*
///   and_expr := not_expr ( ('AND'|'and'|'&&') not_expr )*
///               -- adjacent not_exprs compose as implicit AND
///   not_expr := ('NOT'|'not'|'!') not_expr | atom
///   atom     := '(' or_expr ')' | leaf
///
///   leaf     := column op value
///             | column ('IN'|'in') value_list
///
///   column   := ident | quoted-string
///   op       := ':' | '=' | '~' | '%' | '>' | '>=' | '<' | '<='
///
///   value      := quoted-string | number | 'true' | 'false' | bareword | regex-lit
///   regex-lit  := '/' ... '/'    (only valid after '~')
///   value_list := '[' value (',' value)* ']'
///               | '[' bound? '..' bound? ']'     (range form)
///
/// Leaf semantics:
///   col:val         String Contains
///   col="val"       String Exactly
///   col~/re/        String RegularExpression
///   col%"pat"       String Wildcard
///   col=N           Numeric equal
///   col>N / >=N / <N / <=N   Numeric one-sided range
///   col=true / =false        Boolean
///   col IN [a,b,c]  Enumeration multi-select
///   col IN [a..b]   Numeric or Time range (Time when bounds
///                   parse as ISO timestamps, else Numeric;
///                   either bound may be empty)
///
/// `AND`, `OR`, `NOT`, `IN` are case-insensitive; `FormatExpression`
/// canonicalises to uppercase. Column identity is captured
/// verbatim into `LeafRule::columnKeys` as a single element.
/// Empty / whitespace-only input returns match-all.
[[nodiscard]] std::expected<FilterExpression, QueryParseError> ParseQuery(std::string_view input);

/// Render @p expression back to the query grammar. Round-trips
/// with `ParseQuery` for every tree the parser can produce.
///
/// Notable rules:
///   - `Leaf`: bareword tokens are quoted when they contain
///     whitespace / operator chars or start with a digit; regex
///     leaves always render as `/.../`.
///   - Top-level empty `And` renders as the empty string. Nested
///     empty `And` (never produced by the parser) debug-renders
///     as `*`; empty `Or` debug-renders as `()`.
///   - `Or` is parenthesised when nested under `And` or `Not`;
///     `Not` parenthesises its child unless it's a leaf or `Not`.
[[nodiscard]] std::string FormatExpression(const FilterExpression &expression);

} // namespace loglib
