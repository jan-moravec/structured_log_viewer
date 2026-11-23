/**
 * @brief Parser for CSV files.
 *
 * The first non-blank line is the header and names the columns. The parser follows RFC 4180: comma separators, doubled
 * quotes and CRLF or LF line ends. Detection also needs a second line with the same number of cells, so headerless
 * files fall through to other formats. It supports both the parallel file pipeline and the live-stream path.
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
 * @brief Parses CSV files and streams as RFC 4180 describes: commas only, a doubled quote for a quote, CRLF or LF line
 * ends, and a UTF-8 byte order mark removed from the header.
 *
 * The first non-blank line is the header and names the columns. Detection also requires a second non-blank line with
 * the same number of cells, so a file without a header falls through to the next parser.
 *
 * Unquoted cells are typed by ClassifyBareScalar, as in LogfmtParser: true and false become booleans, decimals become
 * integers, numbers with a dot or an exponent become doubles, and everything else is a string. Quoted cells always stay
 * strings. Empty cells are left out of the row, which saves 16 bytes per cell on wide rows and reads as empty anyway.
 *
 * Limits:
 * - Duplicate header names get a suffix, name_2, name_3 and so on, so each column stays addressable.
 * - A quoted cell that runs over several lines is rejected as an unterminated quoted value.
 * - A stream reads the header once. After a log rotation the new file's header is parsed as data, so stop and reopen to
 *   recover.
 * - With several TCP clients, the first line to arrive sets the columns for all of them. Use one sender, or agree on
 * the header.
 *
 * It uses the same parallel pipeline and streaming loop as the other parsers, through one CsvLineDecoder.
 */
class CsvParser : public LogParser
{
public:
    /**
     * @brief Parses a static file. The header is read first to map the columns to key ids, then the pipeline runs.
     * The decoder records the offset of the header line and emits no row for it.
     */
    void Parse(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /**
     * @brief Parses a live stream. The first non-blank line is the header and produces no row. The lines after it
     * become rows.
     */
    void Parse(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;
};

} // namespace loglib
