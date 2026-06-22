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

/// CSV parser, RFC 4180 strict (comma-only, `""` quote escape, CRLF
/// or LF, UTF-8 BOM stripped on the header).
///
/// The first non-blank line is the header and names the columns;
/// `IsValid` also requires a second non-blank line with the same
/// cell count, so headerless files fall through to the next parser.
///
/// Cell typing (bare cells only) is shared with `LogfmtParser` via
/// `internal::ClassifyBareScalar`: `true`/`false` -> bool, decimal
/// -> `int64`/`uint64`, with `.` or exponent -> double, else string.
/// Quoted cells always stay strings. Empty cells are omitted from
/// the row (lookup yields monostate anyway, and skipping saves 16 B
/// per empty cell on wide rows).
///
/// Known limits:
/// - Duplicate header columns are renamed `<name>_2`, `<name>_3`, ...
///   so both slots remain addressable.
/// - Multi-line quoted cells (RFC 4180 quotes spanning newlines) are
///   rejected as `Unterminated quoted value.`.
/// - Stream Mode latches the schema once; on `logrotate` the new
///   file's header is parsed as data (Stop + reopen to recover).
/// - Network Stream with multiple TCP clients: the first arriving
///   line sets the schema for everyone (coordinate or restrict to a
///   single producer).
///
/// Shares the static TBB pipeline (`DecodeCsvBatch`) and streaming
/// loop (`CsvLineDecoder`) with the other parsers.
class CsvParser : public LogParser
{
public:
    bool IsValid(const std::filesystem::path &file) const override;

    /// Static parse: eagerly parses the header to build the
    /// `column -> KeyId` map, then runs the TBB pipeline; Stage B
    /// records the header line's offset and skips emitting a row
    /// for it.
    void ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /// Live-tail parse. The first non-blank inbound line latches
    /// the header (returned as `Skip`, no row emitted); subsequent
    /// lines append as normal.
    void ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /// Static overload exposing internal tuning knobs (benchmarks / bisects).
    static void ParseStreaming(
        FileLineSource &source,
        LogParseSink &sink,
        const ParserOptions &options,
        internal::AdvancedParserOptions advanced
    );

    /// Render @p line as a single CSV record. Cells are emitted in
    /// `KeyId`-sorted order (matching `LogfmtParser::ToString`)
    /// and RFC-4180-quoted when they contain `,` / `"` / CR / LF.
    /// Header-order round-trip is a follow-up.
    std::string ToString(const LogLine &line) const override;

    /// `LogMap` convenience overload (tests, debug dumps).
    /// Lexicographic by key, mirroring `LogfmtParser::ToString(LogMap)`.
    static std::string ToString(const LogMap &values);
};

} // namespace loglib
