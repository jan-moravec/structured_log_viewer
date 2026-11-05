/**
 * @brief The batch of rows, errors and new keys that a parser hands on.
 *
 * Parsers cut their output into batches. A batch carries the rows, the line offsets of a file, the parse errors and the
 * keys the parser added. The parse module hands each batch to a LogParseSink, the table appends it, and the GUI queues
 * it between threads, so the type lives in core, which all of them may use.
 *
 * This is part of the core module.
 */
#pragma once

#include "loglib/core/log_line.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace loglib
{

/**
 * @brief One problem a parser found, with the physical lines it refers to.
 *
 * Lines are counted from 1 for files and streams alike. Both are 0 when the error has no line, as for a parser that
 * could not start. A single line has firstLine equal to lastLine.
 */
struct ParseError
{
    std::uint64_t firstLine = 0;
    std::uint64_t lastLine = 0;
    std::string message;

    friend bool operator==(const ParseError &, const ParseError &) = default;
};

/**
 * @brief The text shown to users for an error.
 *
 * It is "Error on line N: message" for one line, "Error on lines N-M: message" for a range, and the message alone when
 * the error has no line.
 */
[[nodiscard]] std::string FormatParseError(const ParseError &error);

/**
 * @brief One batch of output that a parser hands to a LogParseSink.
 *
 * A batch without rows is valid, even if it has errors or new keys. The parser always sends a last batch before
 * OnFinished.
 *
 * lines holds the parsed rows, for static files and live streams alike. Each row knows its LineSource, so reading it is
 * the same for both. localLineOffsets is filled only by the file pipeline, and is appended to the line offsets of the
 * LogFile. The streaming loop leaves it empty.
 */
struct RowBatch
{
    std::vector<LogLine> lines;
    std::vector<uint64_t> localLineOffsets;
    std::vector<ParseError> errors;
    std::vector<std::string> newKeys;
    /**
     * @brief The line ranges of multi-line records in a file source. A consumer must append localLineOffsets before it
     * registers these spans on the LogFile.
     */
    std::vector<MultiLineRecordSpan> multiLineSpans;
    /**
     * @brief The line number, counting from 1, where the batch starts.
     *
     * For a batch with lines it is the start of the chunk, which can be lower than the first parsed line when errors
     * come before it. For a batch without lines it is the position of the line counter when the batch was closed, and
     * it does not refer to a particular line.
     */
    size_t firstLineNumber = 0;
    /**
     * @brief The number of the last physical line read when the batch was sent, counting from 1.
     *
     * Every line up to it is accounted for: a row, an error, a blank line, a continuation, or part of a record held
     * for the next batch. It is 0 before the first line. The last batch of a parse holds the total line count, the
     * same for a file and for a stream.
     */
    size_t lastLineNumber = 0;
};

} // namespace loglib
