#pragma once

#include "log_data.hpp"
#include "log_line.hpp"
#include "parser_options.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace loglib
{

class LogFile;
class StreamingLogSink;

namespace internal
{
struct AdvancedParserOptions;
}

/**
 * @brief Result of parsing a log file.
 *
 * This structure contains the parsed log data and errors that occurred during parsing.
 *
 */
struct ParseResult
{
    LogData data;
    std::vector<std::string> errors;
};

/**
 * @brief Interface for log parsers.
 *
 * Concrete parsers implement `IsValid`, `ParseStreaming` (the new pluggability
 * surface), and `ToString`. The synchronous `Parse(path)` overload has a
 * default body that opens a `LogFile`, builds a `BufferingSink`, calls
 * `ParseStreaming(file, sink, ParserOptions{})`, and returns the buffered
 * result. The advanced overload accepts an `internal::AdvancedParserOptions`
 * for benchmarks and bisects.
 *
 */
class LogParser
{
public:
    virtual ~LogParser() = default;

    /**
     * @brief Check if the given file is valid for parsing.
     *
     * This method should be implemented to check if the file can be parsed by this parser.
     *
     * @param file The path to the log file.
     * @return true if the file is valid, false otherwise.
     */
    virtual bool IsValid(const std::filesystem::path &file) const = 0;

    /**
     * @brief Synchronous parse with default options.
     *
     * Default body: opens @p file as a `LogFile`, builds a `BufferingSink`,
     * delegates to `ParseStreaming(file, sink, ParserOptions{})`, and returns
     * `ParseResult{ sink.TakeData(), sink.TakeErrors() }`. Concrete parsers
     * normally do not need to override this — overriding `ParseStreaming` is
     * enough.
     */
    virtual ParseResult Parse(const std::filesystem::path &file) const;

    /**
     * @brief Streams the parse of @p file into @p sink.
     *
     * The new pluggability surface. Concrete parsers must implement this to
     * drive the shared streaming pipeline (or any other implementation
     * producing the same `StreamingLogSink` event sequence).
     *
     * Cooperative cancellation is via `options.stopToken`. When non-null,
     * `options.configuration` enables in-pipeline timestamp promotion for
     * `Type::time` columns.
     */
    virtual void ParseStreaming(LogFile &file, StreamingLogSink &sink, ParserOptions options = {}) const = 0;

    /**
     * @brief Streaming parse with both the public options and the advanced
     *        tuning knobs.
     *
     * Reachable only when the caller includes
     * `<loglib/internal/parser_options.hpp>`. Concrete parsers must
     * implement this overload; the simple `ParseStreaming(file, sink,
     * options)` overload is the user-facing entry point.
     */
    virtual void ParseStreaming(
        LogFile &file,
        StreamingLogSink &sink,
        ParserOptions options,
        internal::AdvancedParserOptions advanced
    ) const = 0;

    /**
     * @brief Convert a log line to a string representation.
     *
     * Implementations should produce the parser's native serialization (e.g. a
     * JSON object string for `JsonParser`). Operates on a `LogLine` rather
     * than the raw value collection so that implementations can resolve KeyIds
     * via the line's bound `KeyIndex`.
     *
     * @param line The log line to convert.
     * @return std::string The string representation of the log line.
     */
    virtual std::string ToString(const LogLine &line) const = 0;
};

} // namespace loglib
