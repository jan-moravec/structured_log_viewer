#pragma once

#include "loglib/key_index.hpp"
#include "loglib/log_data.hpp"
#include "loglib/log_file.hpp"
#include "loglib/streaming_log_sink.hpp"

#include <memory>
#include <string>
#include <vector>

namespace loglib
{

/**
 * @brief Internal `StreamingLogSink` adapter for the synchronous
 *        `LogParser::Parse(path) -> ParseResult` API.
 *
 * Accumulates every line and error from the streaming pipeline into a single
 * in-memory `LogData` and `errors` vector. Lets `Parse(path)` stay a thin
 * wrapper over `ParseStreaming(path, sink, opts)` without forcing the rest
 * of the codebase onto the streaming surface.
 *
 * Constructed by `LogParser::Parse` with an already-opened `LogFile` that
 * the sink transfers into the final `LogData` once streaming completes.
 */
class BufferingSink : public StreamingLogSink
{
public:
    /**
     * @brief Builds a sink that takes ownership of @p logFile and
     *        accumulates batches against the internally-owned `KeyIndex`.
     *
     * One sink per parse — construct a fresh instance per `Parse` call.
     */
    explicit BufferingSink(std::unique_ptr<LogFile> logFile);

    /// Canonical KeyIndex handed to the streaming pipeline. Owned here so
    /// the lifetime trivially outlasts the parse.
    KeyIndex &Keys() override;

    void OnStarted() override;
    void OnBatch(StreamedBatch batch) override;
    void OnFinished(bool cancelled) override;

    /// Opts into the parser's uncoalesced fast path. We re-buffer everything
    /// into our own accumulators anyway, so the default coalescing is just
    /// extra copies for no observable benefit.
    bool PrefersUncoalesced() const override
    {
        return true;
    }

    /// Releases the buffered `LogData` to the caller. Call exactly once
    /// after `OnFinished`.
    LogData TakeData();

    /// Releases the accumulated parse errors. Call exactly once after
    /// `OnFinished`.
    std::vector<std::string> TakeErrors();

private:
    std::unique_ptr<LogFile> mFile;
    KeyIndex mKeys;
    std::vector<LogLine> mLines;
    std::vector<uint64_t> mLineOffsets;
    std::vector<std::string> mErrors;
    bool mFinished = false;
};

} // namespace loglib
