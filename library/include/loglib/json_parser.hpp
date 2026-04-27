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

namespace internal
{
struct AdvancedParserOptions;
}

/// Newline-delimited JSON parser, built on the shared TBB pipeline + simdjson.
class JsonParser : public LogParser
{
public:
    bool IsValid(const std::filesystem::path &file) const override;

    void ParseStreaming(LogFile &file, StreamingLogSink &sink, ParserOptions options = {}) const override;

    /// Streaming parse with the internal tuning knobs (benchmarks / bisects).
    void ParseStreaming(
        LogFile &file,
        StreamingLogSink &sink,
        ParserOptions options,
        internal::AdvancedParserOptions advanced
    ) const;

    std::string ToString(const LogLine &line) const override;

    /// Convenience overload for `LogMap` callers (tests, debug dumps).
    std::string ToString(const LogMap &values) const;
};

} // namespace loglib
