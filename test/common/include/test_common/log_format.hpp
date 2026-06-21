#pragma once

#include <test_common/log_record.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace test_common
{

// A pluggable serializer that turns `LogRecord`s into a format's native
// on-disk text. The same RNG-driven record sequence can be fed through any
// `LogFormat`, keeping benchmark numbers across formats directly comparable.
//
// `writeHeader` is emitted once before the records (empty for self-describing
// formats such as JSON lines / logfmt; populated by schema-bearing formats
// like CSV). `writeLine` serializes one record to a single output line and
// MUST NOT include a trailing newline — the caller is responsible for line
// termination, and a trailing newline would create blank-line records that
// every parser silently skips, hiding fixture-construction bugs from anyone
// reading the benchmark output. Both `TestStructuredLogFile` constructors
// `assert` on this in debug builds.
//
// CROSS-FORMAT TYPE FIDELITY. `LogRecord` is the canonical typed record;
// each `LogFormat` is free to lower types its on-disk syntax cannot express
// to a less-typed surrogate (e.g. logfmt has no notion of nested arrays /
// objects and flattens them to a quoted JSON string; see `Logfmt()` below).
// That keeps lines/s comparable across formats (same field count), but means
// the record-shape a parser observes after a round-trip is not always
// bit-equal to the input. The `[logfmt_parser][round_trip]` test cases in
// `test_logfmt_parser.cpp` pin which value families survive each format's
// round-trip and which downgrade — extend them whenever you add a new
// `LogFormat`.
struct LogFormat
{
    std::string_view suggestedExtension;
    std::function<std::string(const RecordSchema &)> writeHeader;
    std::function<std::string(const LogRecord &)> writeLine;
};

// JSON Lines: one compact JSON object per line. Lossless: every `LogRecord`
// value family round-trips bit-equal through `loglib::JsonParser`.
LogFormat JsonLines();

// logfmt (Heroku / `kr/logfmt` flavour): whitespace-separated `key=value`
// pairs. Fields are emitted in the record's lexicographic key order; values
// are quoted only when they are not bare-safe.
//
// Lossy by design (see `CROSS-FORMAT TYPE FIDELITY` on `LogFormat`):
//   - Nulls serialize as the empty value (`key=`), so a round-tripped null
//     comes back as an empty string at the parser layer.
//   - Booleans / integers / floats serialize to their bare JSON token, so
//     `LogfmtParser` re-types them via its standard bare-value classifier
//     (numeric → `uint64_t` / `int64_t` / `double`; `true` / `false` → bool).
//   - Nested arrays and objects serialize as a single quoted logfmt string
//     containing the compact JSON encoding. Keeps the per-line field count
//     identical to the JSON serialization (so `[logfmt_parser][wide]`'s
//     field-count matches `[json_parser][wide]`), but the parser sees the
//     literal JSON text as an opaque string — *no* structured round-trip.
//
// Keys must be bare-safe (no whitespace, '=', '"', or '\\'); the writer
// asserts on this in debug builds because logfmt has no key-quoting syntax.
LogFormat Logfmt();

} // namespace test_common
