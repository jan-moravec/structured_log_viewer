#pragma once

#include "loglib/filter_expression.hpp"

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace loglib
{

/// One-line parse-time error surfaced by `ParseQuery`. The parser
/// returns the offset (byte, into the input text) where the error
/// was diagnosed and a one-line message. Offsets are stable across
/// callers so the editor can underline the offending token.
struct QueryParseError
{
    /// Zero-based byte offset into the input where the error was
    /// reported. Clamped to `input.size()` for end-of-input errors
    /// so callers can render a caret past the last character.
    std::size_t offset = 0;
    std::string message;
};

/// Parse @p input into a `FilterExpression` tree.
///
/// Grammar (v1):
///
///   query   := or_expr
///   or_expr := and_expr ( ('OR' | 'or' | '||') and_expr )*
///   and_expr:= not_expr ( ('AND' | 'and' | '&&') not_expr )*
///              -- adjacent `not_expr`s without an explicit operator
///              -- also compose under `AND` (implicit-AND).
///   not_expr:= ('NOT' | 'not' | '!') not_expr | atom
///   atom    := '(' or_expr ')' | leaf
///
///   leaf    := column op value
///            | column 'in' value_list
///
///   column  := ident | quoted-string
///   op      := ':' | '=' | '~' | '%' | '>' | '>=' | '<' | '<='
///
///   value   := quoted-string | number | 'true' | 'false' | bareword | regex-lit
///   regex-lit  := '/' ... '/'      -- only valid after '~'
///   value_list := '[' value (',' value)* ']'
///                | '[' bound? '..' bound? ']'    -- range form
///   bound   := number | quoted-string
///
/// Leaf semantics:
///   col:val                     -- String Contains
///   col="val"                   -- String Exactly
///   col~/re/                    -- String RegularExpression
///   col%"pat"                   -- String Wildcard
///   col=N                       -- Numeric equal (min = max = N)
///   col>N / >=N / <N / <=N      -- Numeric one-sided range
///   col=true / =false           -- Boolean single-value
///   col in [a, b, c]            -- Enumeration multi-select
///   col in [a..b]               -- Numeric or Time range (autodetected
///                                  from bound token shape: ISO timestamp
///                                  -> Time, else -> Numeric). Either
///                                  bound may be empty for one-sided.
///
/// Column identity is captured verbatim into `LeafRule::columnKeys`
/// as a single-element vector (subset-matched against
/// `Column::keys` at compile time). Bareword columns match one
/// key. Quoted columns preserve embedded spaces / operators.
///
/// Whitespace between tokens is skipped; the parser tolerates
/// trailing whitespace. An empty (or whitespace-only) input
/// returns the default `FilterExpression` (empty `And` -- match all).
[[nodiscard]] std::expected<FilterExpression, QueryParseError> ParseQuery(std::string_view input);

/// Render @p expression back to the query grammar. Round-trips with
/// `ParseQuery` for every tree the parser can produce.
///
/// Formatting rules:
///   - `Leaf`   -- emits `col op value` per the leaf semantics above.
///                 Bareword columns / values are quoted when they
///                 contain whitespace, operator chars, or start
///                 with a digit; regex leaves always use `/.../`.
///   - `And`    -- children joined with ` AND `. Empty `And` -> `*`
///                 (canonical "match all" spelling).
///   - `Or`     -- children joined with ` OR `. Enclosed in `(...)`
///                 when nested under an `And` or `Not`.
///   - `Not`    -- `NOT <child>`; `<child>` parenthesised unless it's
///                 a leaf or another `Not`.
[[nodiscard]] std::string FormatExpression(const FilterExpression &expression);

} // namespace loglib
