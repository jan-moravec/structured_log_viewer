/**
 * @brief Receiver for the batches a parser produces.
 *
 * Parsers do not build a table themselves. They cut their output into StreamedBatch values and hand each one to a
 * LogParseSink, which can append it to a table, buffer it for a synchronous call, or discard it. The sink gets a start
 * call, any number of batches and a finish call that says whether parsing was cancelled.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/key_index.hpp"
#include "loglib/core/log_line.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace loglib
{

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
struct StreamedBatch
{
    std::vector<LogLine> lines;
    std::vector<uint64_t> localLineOffsets;
    std::vector<std::string> errors;
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
};

/**
 * @brief Receives the output of a parser.
 *
 * One serial worker calls the sink, always in this order:
 * 1. OnStarted, once.
 * 2. OnBatch, at least once. The last batch can be empty, and a parse of an empty or cancelled source still sends one,
 *    so a sink that sets itself up on the first batch behaves the same in every case.
 * 3. OnFinished, once, with cancelled set when the parse was stopped through ParserOptions::stopToken.
 *
 * The same interface serves a synchronous parse of a static file and live streaming, where a sink hands each batch to
 * a consumer on another thread.
 */
class LogParseSink
{
public:
    virtual ~LogParseSink() = default;

    /**
     * @brief The key index the parser adds keys to. Parser threads use it at the same time, and it must stay the same
     * object from OnStarted to OnFinished.
     */
    virtual KeyIndex &Keys() = 0;

    virtual void OnStarted() = 0;

    virtual void OnBatch(StreamedBatch batch) = 0;

    virtual void OnFinished(bool cancelled) = 0;
};

} // namespace loglib
