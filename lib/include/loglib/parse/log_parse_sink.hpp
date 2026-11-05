/**
 * @brief Receiver for the batches a parser produces.
 *
 * Parsers do not build a table themselves. They cut their output into RowBatch values and hand each one to a
 * LogParseSink, which can append it to a table, buffer it for a synchronous call, or discard it. The sink gets a start
 * call, any number of batches and a finish call that says whether parsing was cancelled.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/key_index.hpp"
#include "loglib/core/row_batch.hpp"

namespace loglib
{

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

    virtual void OnBatch(RowBatch batch) = 0;

    virtual void OnFinished(bool cancelled) = 0;
};

} // namespace loglib
