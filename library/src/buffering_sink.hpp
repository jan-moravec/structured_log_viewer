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
 * @brief Internal `StreamingLogSink` adapter used by the legacy
 *        `JsonParser::Parse(path) -> ParseResult` API.
 *
 * Accumulates every line and error emitted by the streaming pipeline into a
 * single in-memory `LogData` and `errors` vector that the legacy synchronous
 * API hands back to its caller. This keeps `Parse(path)` a thin wrapper over
 * `ParseStreaming(path, sink, opts)` without forcing the rest of the codebase
 * onto the streaming surface immediately (PRD req. 4.3.25, 4.4.31).
 *
 * Constructed by `JsonParser::Parse` with the `LogFile` already opened so the
 * sink can transfer it into the final `LogData` once streaming completes.
 */
class BufferingSink : public StreamingLogSink
{
public:
    /**
     * @brief Builds a sink that takes ownership of @p logFile and accumulates
     *        all batches against the canonical KeyIndex it owns internally.
     *
     * The sink is intended for one parse: callers should construct a fresh
     * instance per `Parse` invocation.
     */
    explicit BufferingSink(std::unique_ptr<LogFile> logFile);

    /**
     * @brief Returns the canonical KeyIndex the sink hands to streaming Stage
     *        B/C filters. Must outlive the parse — owned here on purpose.
     */
    KeyIndex &Keys() override;

    void OnStarted() override;
    void OnBatch(StreamedBatch batch) override;
    void OnFinished(bool cancelled) override;

    /**
     * @brief Opts the buffering sink into Stage C's uncoalesced fast path.
     *
     * Per PRD §4.8.3 / parser-perf task 9.4: `BufferingSink::OnBatch`
     * unconditionally appends every batch into its own `mLines` /
     * `mLineOffsets` / `mErrors` accumulators, so Stage C's 1 000-line
     * coalescing window is wasted double-buffering. Returning `true`
     * lets the parser forward each `ParsedPipelineBatch` directly to
     * `OnBatch` without going through the `pending` accumulator or the
     * `kStreamFlushLines` / `kStreamFlushInterval` thresholds. Functional
     * behaviour of the legacy `Parse(path)` API is unchanged — only the
     * Stage C bookkeeping cost goes away.
     */
    bool PrefersUncoalesced() const override
    {
        return true;
    }

    /**
     * @brief Releases the buffered `LogData` (file + lines + keys) to the
     *        caller. Must be called exactly once after `OnFinished`.
     */
    LogData TakeData();

    /**
     * @brief Releases the accumulated parse errors to the caller. Must be
     *        called exactly once after `OnFinished`.
     */
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
