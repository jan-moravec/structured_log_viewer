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

// Per-template synthesizers for the most common shipped
// `RegexTemplate`s. Each factory returns a `LogFormat` whose
// `writeLine` emits a line that the corresponding template pattern
// in `loglib::BuiltinRegexTemplates()` parses without error. They
// reuse the canonical fields `GenerateRandomLogRecord` populates
// (`timestamp` / `level` / `message`) and synthesize any format
// extras (client IP, HTTP verb, logger, pid, ...) from small static
// pools via a per-line RNG seeded from `line_number` — so runs
// stay deterministic under a pinned seed.
//
// The `test_regex_template_generators` round-trip tests guard
// against drift between each factory's output and its template.

// `Syslog (RFC3164)` -> "MMM DD HH:MM:SS <host> <program>[<pid>]: <message>".
LogFormat SyslogRfc3164Format();

// `Apache/nginx Combined Log Format` -> Apache/nginx CLF + referrer + agent.
LogFormat ApacheCombinedFormat();

// `Apache/nginx Common Log Format` -> Apache/nginx CLF without the trailing
// referrer / user-agent fields.
LogFormat ApacheCommonFormat();

// `Apache error log` -> "[<Www Mon DD HH:MM:SS YYYY>] [<module>:<level>]
// [pid <n>] [client <ip>:<port>] <message>".
LogFormat ApacheErrorFormat();

// `Java / log4j / SLF4J Logback` -> "YYYY-MM-DD HH:MM:SS,mmm LEVEL
// [<thread>] <logger> - <message>". Levels normalised to SLF4J tokens.
LogFormat JavaLogFormat();

// Lex-ordered list of @p record's object keys; empty if @p record
// is not an object. Used to derive a CSV header from a sample record.
RecordSchema DeriveSchemaFromRecord(const LogRecord &record);

} // namespace test_common
