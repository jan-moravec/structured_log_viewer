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

/// Newline-delimited logfmt parser.
///
/// Logfmt has no formal grammar; the de-facto reference is the
/// `kr/logfmt` Go scanner (see `library/src/parsers/logfmt_parser.cpp`
/// for the cited port). One record per line, whitespace-separated
/// `key=value` pairs, with optional double-quoted values and C-style
/// escapes (`\"`, `\\`, `\n`, `\r`, `\t`).
///
/// Bare values are typed: empty (`key=`) -> null, `true`/`false` ->
/// bool, decimal integer -> int64/uint64, otherwise number-or-string.
/// Quoted values stay strings (preserves user intent like
/// `pid="42"`).
///
/// Reuses the same TBB static pipeline + streaming loop as
/// `JsonParser`. The decoder satisfies `internal::CompactLineDecoder`
/// so both overloads share one tokeniser implementation.
class LogfmtParser : public LogParser
{
public:
    bool IsValid(const std::filesystem::path &file) const override;

    /// Static-file streaming parse over @p source's mmap. Each emitted
    /// `LogLine` carries `&source` and its 0-based file-line id.
    void ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /// Live-tail streaming parse. Each line read from
    /// `source.Producer()` is appended via `AppendLine` and surfaced
    /// as a `LogLine` carrying `&source` and the new 1-based id.
    void ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /// Static-file overload exposing internal tuning knobs (used by
    /// benchmarks / bisects).
    static void ParseStreaming(
        FileLineSource &source,
        LogParseSink &sink,
        const ParserOptions &options,
        internal::AdvancedParserOptions advanced
    );

    std::string ToString(const LogLine &line) const override;

    /// Convenience for `LogMap` (tests, debug dumps).
    static std::string ToString(const LogMap &values);
};

} // namespace loglib
