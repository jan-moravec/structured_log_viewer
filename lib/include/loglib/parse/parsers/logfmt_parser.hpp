/**
 * @brief Parser for logfmt.
 *
 * Each line holds key=value pairs separated by spaces, and values may be double quoted with escapes. By default, an
 * indented line continues the previous record; ParserOptions can turn that off. It supports both the parallel file
 * pipeline and the live-stream path.
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
 * @brief Parses files and streams in the logfmt format, one record per line, as used by Heroku and the logfmt library.
 *
 * A record is a list of key=value pairs separated by whitespace. A value can be in double quotes with C-style escapes,
 * such as an escaped quote, a backslash, \n, \r and \t. By default an indented line continues the previous record. Set
 * ParserOptions::multilineLogfmt to false for one record per line.
 *
 * Unquoted values are typed by ClassifyBareScalar: true and false become booleans, decimals become integers, numbers
 * with a point or an exponent become doubles, and everything else is a string. Quoted values stay strings even when
 * they look like numbers, so pid="42" keeps what the writer meant.
 *
 * It uses the same parallel pipeline and streaming loop as JsonParser, through CompactLineDecoder.
 */
class LogfmtParser : public LogParser
{
public:
    /**
     * @brief Says whether the first non-blank line of the sample looks like key=value pairs (LineLooksLikeLogfmt).
     *
     * Called through the file overload, it only sees the first PROBE_BYTES_BUDGET bytes.
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
