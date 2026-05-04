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

/// Newline-delimited JSON parser, built on the shared TBB pipeline + simdjson.
class JsonParser : public LogParser
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
    void ParseStreaming(
        FileLineSource &source, LogParseSink &sink, ParserOptions options, internal::AdvancedParserOptions advanced
    ) const;

    std::string ToString(const LogLine &line) const override;

    /// Convenience for `LogMap` (tests, debug dumps).
    std::string ToString(const LogMap &values) const;
};

} // namespace loglib
