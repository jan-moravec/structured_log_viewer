#pragma once

#include "../log_line.hpp"
#include "../log_parse_sink.hpp"
#include "../log_parser.hpp"
#include "../parser_options.hpp"

#include <filesystem>
#include <string>

namespace loglib
{

class FileLineSource;
class StreamLineSource;

namespace internal
{
struct AdvancedParserOptions;
}

/// Newline-delimited CSV parser (RFC 4180 strict, comma-only).
///
/// One record per line: comma-separated fields, with `"`-quoted
/// fields supporting `""` as an escape for a literal quote.
/// CRLF and bare LF are both accepted as line terminators; a
/// leading UTF-8 BOM on the header is stripped.
///
/// The **first non-blank line is the header** and names the
/// columns. `IsValid` requires it (and a second non-blank line
/// with a matching tokenised field count), so files without a
/// header fall through to the next parser. Headerless support is
/// a clean follow-up if real fixtures need it.
///
/// Bare cells are typed via `internal::ClassifyBareScalar`
/// (shared with `LogfmtParser`): empty -> not stored, `true`/`false`
/// -> bool, decimal -> `int64`/`uint64`, decimal-with-point /
/// exponent -> double, else string. Quoted cells stay strings
/// even if numeric (so `"42"` keeps the author's intent), and
/// `""`-escaped quoted cells are copied into the per-batch owned
/// arena. Empty cells (`a,,c` or missing trailing cells) are
/// **omitted** from the row, not stored as monostate -- an absent
/// key already materialises as monostate on lookup, and skipping
/// the slot avoids 16 B per empty cell on wide CSV rows.
///
/// Multi-line quoted cells (an RFC-4180 quoted field that spans
/// newlines) are **rejected** in v1; they surface as
/// `Unterminated quoted value.` on the line where the quote
/// opens. Documented limitation; a follow-up may add real
/// multi-line support if fixtures justify it.
///
/// Reuses the same TBB static pipeline (via `DecodeCsvBatch`) and
/// streaming loop (via `CsvLineDecoder`) as `JsonParser` and
/// `LogfmtParser`.
class CsvParser : public LogParser
{
public:
    bool IsValid(const std::filesystem::path &file) const override;

    /// Static parse over @p source's mmap. The header (first
    /// non-blank line) is parsed eagerly to build the
    /// `column index -> KeyId` map, then the file is fed through
    /// the standard TBB pipeline; Stage B of batch 0 skips
    /// emitting a `LogLine` for the header while still pushing
    /// its line offset so `LogFile::GetLine(lineId)` stays
    /// aligned. Each emitted `LogLine` carries `&source` and its
    /// 0-based file-line id (the header is line 0; the first data
    /// row is line 1, displayed as "line 2" in error messages).
    void ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /// Live-tail parse. The first non-blank inbound line is
    /// latched as the header (decoded internally with `Skip`, so
    /// no `LogLine` is emitted); subsequent lines append via
    /// `AppendLine` and emit `LogLine`s carrying `&source` and
    /// the new 1-based id.
    void ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /// Static overload exposing internal tuning knobs (benchmarks / bisects).
    static void ParseStreaming(
        FileLineSource &source,
        LogParseSink &sink,
        const ParserOptions &options,
        internal::AdvancedParserOptions advanced
    );

    /// Renders @p line back to a single CSV record line. v1
    /// stateless contract: cells are emitted in `KeyId`-sorted
    /// order (matching `LogfmtParser::ToString(LogLine)`'s
    /// shape), RFC-4180-quoting any cell that contains
    /// `,` / `"` / `\r` / `\n`. Header-order round-trip is a
    /// follow-up tracked in the plan.
    std::string ToString(const LogLine &line) const override;

    /// `LogMap` convenience overload (tests, debug dumps).
    /// Lexicographic by key (no schema available here), mirrors
    /// `LogfmtParser::ToString(LogMap)`.
    static std::string ToString(const LogMap &values);
};

} // namespace loglib
