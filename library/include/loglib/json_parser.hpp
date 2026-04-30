#pragma once

#include "log_line.hpp"
#include "log_parser.hpp"
#include "parser_options.hpp"
#include "streaming_log_sink.hpp"

#include <filesystem>
#include <string>

namespace loglib
{

class LogFile;
class LogSource;

namespace internal
{
struct AdvancedParserOptions;
}

/// Newline-delimited JSON parser, built on the shared TBB pipeline + simdjson.
class JsonParser : public LogParser
{
public:
    bool IsValid(const std::filesystem::path &file) const override;

    /// `LogSource`-based streaming parse. The `MappedFileSource` fast path
    /// dispatches to the existing TBB pipeline over the mmap (preserving
    /// the static-path performance bar); other source types route through
    /// the line-by-line streaming loop (task 2.5).
    void ParseStreaming(LogSource &source, StreamingLogSink &sink, ParserOptions options = {}) const override;

    // Bring the base-class `ParseStreaming(LogFile&, ...)` non-virtual
    // wrapper into the derived scope so callers (tests, MainWindow,
    // benchmarks) can keep writing `parser.ParseStreaming(file, sink,
    // opts)` without having to qualify with `LogParser::`. C++ name-hiding
    // would otherwise occlude every base-class overload as soon as we
    // declared the source-based `ParseStreaming` here.
    using LogParser::ParseStreaming;

    /// Streaming parse with the internal tuning knobs (benchmarks / bisects).
    /// Drives the TBB pipeline directly over @p file's mmap; bypasses the
    /// `LogSource` indirection because the advanced knobs are
    /// pipeline-specific.
    void ParseStreaming(
        LogFile &file, StreamingLogSink &sink, ParserOptions options, internal::AdvancedParserOptions advanced
    ) const;

    std::string ToString(const LogLine &line) const override;

    /// Convenience overload for `LogMap` callers (tests, debug dumps).
    std::string ToString(const LogMap &values) const;
};

} // namespace loglib
