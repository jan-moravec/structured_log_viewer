#pragma once

#include "../log_line.hpp"
#include "../log_parser.hpp"
#include "../parser_options.hpp"
#include "../streaming_log_sink.hpp"

#include <filesystem>
#include <string>

namespace loglib
{

class FileLineSource;
class LogFile;
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

    /// Static-file streaming parse against a long-lived `FileLineSource`.
    /// Drives the TBB pipeline directly over @p source's `LogFile` mmap;
    /// each `LogLine` produced is tagged with `&source` and its absolute
    /// 0-based file-line id. Default `AdvancedParserOptions` are used.
    void
    ParseStreaming(FileLineSource &source, StreamingLogSink &sink, ParserOptions options = {}) const override;

    /// Live-tail streaming parse against a long-lived `StreamLineSource`.
    /// Each line read from `source.Producer()` is appended to @p source
    /// and surfaced as a `LogLine` tagged with `&source` and the
    /// just-published 1-based monotonic line id (PRD 4.10.4).
    void ParseStreaming(StreamLineSource &source, StreamingLogSink &sink, ParserOptions options = {})
        const override;

    // Bring the base-class `ParseStreaming(LogFile&, ...)` non-virtual
    // wrapper into the derived scope so callers (tests, MainWindow,
    // benchmarks) can keep writing `parser.ParseStreaming(file, sink,
    // opts)` without having to qualify with `LogParser::`. C++ name-hiding
    // would otherwise occlude every base-class overload as soon as we
    // declared the source-based `ParseStreaming` here.
    using LogParser::ParseStreaming;

    /// Streaming parse with the internal tuning knobs (benchmarks / bisects).
    /// Same semantics as the 3-arg overload, plus an
    /// `AdvancedParserOptions` knob bundle that is otherwise hidden from
    /// the public API.
    void ParseStreaming(
        FileLineSource &source,
        StreamingLogSink &sink,
        ParserOptions options,
        internal::AdvancedParserOptions advanced
    ) const;

    std::string ToString(const LogLine &line) const override;

    /// Convenience overload for `LogMap` callers (tests, debug dumps).
    std::string ToString(const LogMap &values) const;
};

} // namespace loglib
