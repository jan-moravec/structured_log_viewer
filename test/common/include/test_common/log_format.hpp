#pragma once

#include <test_common/log_record.hpp>

#include <functional>
#include <string>
#include <string_view>

namespace test_common
{

// Pluggable serializer turning `LogRecord`s into a format's on-disk text.
// Feeding the same record sequence through different `LogFormat`s keeps
// cross-format benchmark numbers directly comparable.
//
// `writeHeader` runs once before any records (empty for self-describing
// formats; non-empty for schema-bearing ones like CSV). `writeLine`
// serializes one record per output line and MUST NOT add a trailing
// newline — the caller terminates the line. `TestStructuredLogFile`
// asserts this in debug builds.
//
// Formats may lower types their syntax cannot express (e.g. logfmt
// flattens nested arrays/objects to a quoted JSON string). The
// `[logfmt_parser][round_trip]` tests pin which value families survive
// each format; extend them when adding a new `LogFormat`.
struct LogFormat
{
    std::string_view suggestedExtension;
    std::function<std::string(const RecordSchema &)> writeHeader;
    std::function<std::string(const LogRecord &)> writeLine;
};

// JSON Lines: one compact JSON object per line. Lossless round-trip
// through `loglib::JsonParser`.
LogFormat JsonLines();

// logfmt (Heroku / `kr/logfmt` flavour): whitespace-separated `key=value`
// pairs in lexicographic key order, quoted only when not bare-safe.
//
// Lossy: nulls become empty values (`key=`); nested arrays/objects are
// emitted as a quoted JSON string (matching field count, not structure).
// Booleans and numbers serialize bare and are re-typed by the parser.
//
// Keys must be bare-safe (no whitespace / '=' / '"' / '\\'); the writer
// asserts this in debug builds since logfmt has no key-quoting syntax.
LogFormat Logfmt();

// CSV (RFC 4180 strict, comma-only). Each record emits cells in
// @p schema order, RFC-4180-quoting any cell with `,` / `"` / CR / LF.
// `writeHeader` returns the schema as a CSV header line.
//
// Lossy: nested arrays/objects emit as a quoted compact-JSON cell.
// Bools / numbers emit bare. Null and missing keys render as the
// empty cell (which `CsvParser` then omits from the row).
//
// An empty @p schema produces a headerless CSV-shaped stream walked
// in the record's lex order; real fixtures should pass a schema.
LogFormat Csv(RecordSchema schema = {});

// Bracketed text shape suitable for regex-template extraction. Each
// record emits one line of the form:
//   `[<timestamp>] <level> <component> tid=<thread_id> | <message>`
//
// Lossy: only the five canonical fields from `GenerateRandomLogRecord`
// (`timestamp` / `level` / `component` / `thread_id` / `message`)
// survive; other fields are dropped. Use with `loglib::RegexParser`
// and `BracketedRegexPattern()` for a directly comparable cross-format
// benchmark.
//
// Field constraints (asserted in debug builds): `level` / `component`
// must be bare non-space tokens, `thread_id` must serialize to
// digits-only, `timestamp` must not contain `]`, and `message` must
// not contain a newline. The fixture generator already satisfies all
// of these.
LogFormat BracketedRegex();

// PCRE2 pattern that pairs with `BracketedRegex()`. Five named
// groups -- `timestamp`, `level`, `component`, `thread_id`,
// `message` -- map onto the same column names that the other
// formats produce, so cross-format lines/s comparisons line up.
std::string_view BracketedRegexPattern();

// Lex-ordered list of @p record's object keys; empty if @p record
// is not an object. Used to derive a CSV header from a sample record.
RecordSchema DeriveSchemaFromRecord(const LogRecord &record);

} // namespace test_common
