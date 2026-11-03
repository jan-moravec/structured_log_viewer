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
 * @brief One unit of work handed from the parser to a LogParseSink. A
 * "rows-empty" batch with non-empty errors/newKeys is valid; the parser
 * always emits a final batch before OnFinished.
 *
 * lines carries every parsed log row regardless of session type (static
 * file or live tail): each LogLine is tagged with its LineSource * so
 * resolution stays uniform. localLineOffsets is only populated by the
 * file-source pipeline and is forwarded to the LogFile's line-offset
 * table; the live-tail loop leaves it empty.
 */
struct StreamedBatch
{
    std::vector<LogLine> lines;
    std::vector<uint64_t> localLineOffsets;
    std::vector<std::string> errors;
    std::vector<std::string> newKeys;
    /**
     * @brief File-backed multi-line spans. Consumers must append
     * localLineOffsets before registering these on LogFile.
     */
    std::vector<MultiLineRecordSpan> multiLineSpans;
    /**
     * @brief 1-based absolute line number of the batch's start cursor.
     * - When lines is non-empty: matches the chunk start, not necessarily
     *   the first parsed line (errors preceding it can push it lower).
     * - When lines is empty: the line cursor at the time the batch was
     *   sealed; not tied to any specific source line.
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
