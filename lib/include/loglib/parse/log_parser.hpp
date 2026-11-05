/**
 * @brief Base class for log format parsers.
 *
 * A parser can check whether a buffer or file looks like its format and can parse a static file or a live stream into a
 * sink. New formats derive from LogParser. The synchronous helper ParseFile in parse_file.hpp wraps this interface for
 * callers that just want the rows.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/log_data.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/row_batch.hpp"
#include "loglib/parse/parser_options.hpp"

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{

class FileLineSource;
class LogParseSink;
class StreamLineSource;

/** @brief Buffered output of a synchronous parse. */
struct ParseResult
{
    LogData data;
    std::vector<ParseError> errors;
};

/**
 * @brief How many bytes of a file or stream format detection looks at. Every kind of input uses the same budget, so
 * the same bytes give the same result.
 */
inline constexpr std::size_t PROBE_BYTES_BUDGET = 16 * 1024;

/**
 * @brief Base class of the log format parsers.
 *
 * A new format implements IsValidBytes and both ParseStreaming overloads. IsValid for a path is not virtual. It reads
 * the head of the file and calls IsValidBytes. To parse a whole file at once, see ParseFile. The application calls
 * ParseStreaming directly.
 */
class LogParser
{
public:
    virtual ~LogParser() = default;

    /**
     * @brief Says whether the sample looks like this parser's format.
     *
     * An implementation must stay inside the sample and treat a last line without a newline like a complete one,
     * because the sample can be cut off by the probe budget. Detection and AutoDetectParser both call it. It has its
     * own name so that a path cannot pick it by accident.
     */
    virtual bool IsValidBytes(std::string_view sniffBuffer) const = 0;

    /**
     * @brief Reads the head of a file, up to PROBE_BYTES_BUDGET bytes, and returns IsValidBytes for it. A file that is
     * missing or unreadable gives false.
     */
    bool IsValid(const std::filesystem::path &file) const;

    /**
     * @brief Parses a static file and sends the result to the sink. Rows refer to source, and their line ids are
     * zero-based line numbers of the file.
     */
    virtual void ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const = 0;

    /**
     * @brief Parses a live stream until it ends or options.stopToken fires. Rows refer to source, and their line ids
     * are the ones AppendLine assigned, starting at 1.
     */
    virtual void ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const = 0;
};

/**
 * @brief Reads up to budget bytes from the start of a file. A file that is missing or unreadable gives an empty
 * string.
 */
[[nodiscard]] std::string ReadProbeHead(const std::filesystem::path &file, std::size_t budget = PROBE_BYTES_BUDGET);

} // namespace loglib
