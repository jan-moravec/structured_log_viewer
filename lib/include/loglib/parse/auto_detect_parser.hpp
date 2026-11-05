/**
 * @brief Parser that works out the log format from the first bytes it sees.
 *
 * Network streams have no file name or extension to go by. AutoDetectParser reads a peek buffer from the producer, runs
 * the shared format detection on it, and hands the work to the parser it picked. The bytes it consumed go back to the
 * source as unread bytes, so none are lost. It only supports the live-stream path.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/log_line.hpp"
#include "loglib/parse/format_detection.hpp"
#include "loglib/parse/log_parser.hpp"
#include "loglib/parse/parser_options.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace loglib
{

class FileLineSource;
class LogParseSink;
class StreamLineSource;

/**
 * @brief A parser for streams whose format is not known yet. It works out the format from the first bytes and then
 * hands the stream to the matching parser.
 *
 * It reads a few bytes from the producer, runs the same detection as for static files, and passes the bytes it read to
 * the chosen parser through StreamLineSource::UnreadBytes, so none are lost. Use it for network streams and other live
 * input of unknown format.
 *
 * For a file source it detects from the head of the file and delegates. Static file opens use DetectFormatForPath
 * directly, so that overload exists to satisfy the LogParser interface.
 */
class AutoDetectParser : public LogParser
{
public:
    /**
     * @brief The default time limit for reading the first bytes. It starts when the first byte arrives.
     *
     * A producer that delivers bytes slowly still gets its parser after this long. A producer that has not sent
     * anything yet does not start the clock. The value is the same as the timeout of StdinPeek.
     */
    static constexpr auto DEFAULT_PEEK_TIMEOUT = std::chrono::milliseconds(500);

    /**
     * @param peekBytes How many bytes to read from the producer for detection. The default is the budget every other
     * detector uses.
     * @param peekTimeout The time limit for reading, counted from the first byte. Zero or less turns the limit off,
     * which tests use to read the whole producer. Reading also stops early once the bytes are enough to decide.
     * @param formatOptions Settings for the parser it picks. Only multilineLogfmt is used, because detection chooses
     * the regex pattern.
     */
    explicit AutoDetectParser(
        std::size_t peekBytes = PROBE_BYTES_BUDGET,
        std::chrono::milliseconds peekTimeout = DEFAULT_PEEK_TIMEOUT,
        FormatOptions formatOptions = {}
    ) noexcept;

    /** @brief Always true. The format is chosen in ParseStreaming and not here. */
    bool IsValidBytes(std::string_view sniffBuffer) const override;

    /** @brief Detects the format from the start of the file and lets the matching parser handle it. */
    void ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /**
     * @brief Detects the format of a stream and lets the matching parser handle it.
     *
     * The unread bytes of the source and up to peekBytes bytes from the producer form the sample, and the matching
     * parser starts with all of it. Reading follows the producer's WaitForBytes and stops when options.stopToken
     * fires. It waits for the first byte without a time limit. If stop or the end of the stream leaves no bytes, the
     * JSON parser runs, so the sink still gets its usual calls.
     */
    void ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

private:
    std::size_t mPeekBytes;
    std::chrono::milliseconds mPeekTimeout;
    FormatOptions mFormatOptions;
};

} // namespace loglib
