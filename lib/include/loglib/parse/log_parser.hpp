/**
 * @brief Base class for log format parsers.
 *
 * A parser reads a static file or a live stream and sends the rows to a sink. New formats derive from LogParser. LoadFile in output/file_loading.hpp and ParseIntoTable in
 * table/table_loader.hpp run a parser and fill a table.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/log_data.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/row_batch.hpp"
#include "loglib/parse/parser_options.hpp"

namespace loglib
{

class FileLineSource;
class LogParseSink;
class StreamLineSource;

/**
 * @brief Base class of the log format parsers.
 *
 * A new format implements both Parse overloads. Which parser fits an input is decided by DetectFormat in
 * parse/format_detection.hpp. To load a whole file into a table, see LoadFile. The application calls Parse directly.
 */
class LogParser
{
public:
    virtual ~LogParser() = default;

    /**
     * @brief Parses a static file and sends the result to the sink. Rows refer to source, and their line ids are
     * zero-based line numbers of the file.
     */
    virtual void Parse(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const = 0;

    /**
     * @brief Parses a live stream until it ends or options.stopToken fires. Rows refer to source, and their line ids
     * are the ones AppendLine assigned, starting at 1.
     */
    virtual void Parse(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const = 0;
};

} // namespace loglib
