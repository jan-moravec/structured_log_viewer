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

// CSV (RFC 4180 strict, comma-only). Schema-bearing: every record
// emits cells in @p schema order, RFC-4180-quoting any cell that
// contains `,` / `"` / `\r` / `\n`. The captured @p schema is also
// what `writeHeader(_)` returns; `TestStructuredLogFile` passes the
// same schema to both `Csv(schema)` and its own ctor for clarity.
//
// Lossy: nested arrays/objects emit as a quoted compact-JSON cell
// (matching field count, not structure). Booleans / numbers / nulls
// emit bare so `loglib::CsvParser` re-types them back; null cells
// render as the empty cell (which `CsvParser` then omits from the
// row). Missing keys in a record also render as the empty cell.
//
// When @p schema is empty: `writeHeader` returns the empty string
// and `writeLine` walks the record's lex order (so calling with no
// schema produces a headerless CSV-shaped stream of rows). Real
// fixtures should always pass a non-empty schema.
LogFormat Csv(RecordSchema schema = {});

// Lex-ordered list of object keys from @p record. `LogRecord` is
// `glz::generic_sorted_u64`, whose object iteration is lex-sorted,
// so the result is deterministic. Used to derive a CSV header from
// a sample record. Returns an empty schema if @p record isn't an
// object.
RecordSchema DeriveSchemaFromRecord(const LogRecord &record);

} // namespace test_common
