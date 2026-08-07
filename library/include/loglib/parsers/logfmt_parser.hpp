#pragma once

#include "../log_line.hpp"
#include "../log_parse_sink.hpp"
#include "../log_parser.hpp"
#include "../parser_options.hpp"

#include <string>
#include <string_view>

namespace loglib
{

class FileLineSource;
class StreamLineSource;

namespace internal
{
struct AdvancedParserOptions;
}

/// Newline-delimited logfmt parser (Heroku / `kr/logfmt` flavour).
///
/// Records contain whitespace-separated `key=value` pairs and optional
/// double-quoted values with C-style escapes (`\"`, `\\`, `\n`, `\r`,
/// `\t`). By default, indented lines continue the preceding record;
/// set `ParserOptions::multilineLogfmt` to false for line-per-record parsing.
///
/// Bare values are typed: `key=` -> null, `true`/`false` -> bool,
/// decimal -> int64/uint64, decimal-with-point/exponent -> double,
/// else string. Quoted values stay strings even if numeric (so
/// `pid="42"` keeps the user's intent).
///
/// Reuses the same TBB static pipeline and streaming loop as
/// `JsonParser` via `internal::CompactLineDecoder`.
class LogfmtParser : public LogParser
{
public:
    /// Probe: the first non-blank line of @p sniffBuffer must look
    /// like `key=value` pairs (see `LineLooksLikeLogfmt`). Bounded
    /// by `PROBE_BYTES_BUDGET` when called via the file shim.
    bool IsValidBytes(std::string_view sniffBuffer) const override;

    /// Static parse over @p source's mmap. Each emitted `LogLine`
    /// carries `&source` and its 0-based file-line id.
    void ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /// Live-tail parse. Each line from `source.Producer()` is
    /// appended via `AppendLine` and emitted as a `LogLine`
    /// carrying `&source` and the new 1-based id.
    void ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /// Static overload exposing internal tuning knobs (benchmarks / bisects).
    static void ParseStreaming(
        FileLineSource &source,
        LogParseSink &sink,
        const ParserOptions &options,
        internal::AdvancedParserOptions advanced
    );

    std::string ToString(const LogLine &line) const override;

    /// `LogMap` convenience overload (tests, debug dumps).
    static std::string ToString(const LogMap &values);
};

} // namespace loglib
