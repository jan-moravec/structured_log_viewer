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

/**
 * @brief Parses files and streams with one JSON object per line.
 *
 * Static files go through the parallel pipeline, and live streams through the streaming loop. Decoding uses simdjson.
 */
class JsonParser : public LogParser
{
public:
    /**
     * @brief Says whether the first non-blank line of the sample is a JSON object.
     *
     * The file overload of IsValid, AutoDetectParser and the detection of standard input all use this.
     */
    bool IsValidBytes(std::string_view sniffBuffer) const override;

    /**
     * @brief Parses a static file. Every row refers to source and has the zero-based line number of the file as its
     * id.
     */
    void ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /**
     * @brief Parses a live stream. Every line from the producer is stored with AppendLine, and the row refers to
     * source with the id AppendLine returned, counting from 1.
     */
    void ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;
};

} // namespace loglib
