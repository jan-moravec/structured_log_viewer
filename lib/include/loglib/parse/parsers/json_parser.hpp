/**
 * @brief Parser for newline-delimited JSON.
 *
 * Each line is one JSON object whose members become fields. Parsing uses simdjson inside the shared parallel pipeline
 * for files and a line loop for live streams.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/log_line.hpp"
#include "loglib/parse/log_parse_sink.hpp"
#include "loglib/parse/log_parser.hpp"
#include "loglib/parse/parser_options.hpp"

#include <string>
#include <string_view>

namespace loglib
{

class FileLineSource;
class StreamLineSource;

/** @brief Newline-delimited JSON parser, built on the shared TBB pipeline + simdjson. */
class JsonParser : public LogParser
{
public:
    /**
     * @brief Probe: the first non-blank line of @p sniffBuffer must parse
     * as a JSON object. Shared by the file-based IsValid shim
     * on the base class, by AutoDetectParser for network
     * streams, and by stdin auto-detect.
     */
    bool IsValidBytes(std::string_view sniffBuffer) const override;

    /**
     * @brief Static-file streaming parse over @p source's mmap. Each emitted
     * LogLine carries &source and its 0-based file-line id.
     */
    void ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /**
     * @brief Live-tail streaming parse. Each line read from
     * source.Producer() is appended via AppendLine and surfaced
     * as a LogLine carrying &source and the new 1-based id.
     */
    void ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;
};

} // namespace loglib
