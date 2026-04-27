#pragma once

#include "key_index.hpp"
#include "log_line.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace loglib
{

/**
 * @brief One unit of work handed from the streaming parser to an installed
 *        `StreamingLogSink`.
 *
 * A batch is the parser's only externally observable accumulation point; the
 * sink learns about new lines, new keys, and parse errors exclusively through
 * these objects.
 *
 * Field semantics:
 * - `lines`: parsed `LogLine`s, already bound to the canonical `KeyIndex`.
 *   Move-out on receipt; the parser will not touch them again.
 * - `localLineOffsets`: absolute byte offsets of each line within the source
 *   file, indexed in the same order as `lines`.
 * - `errors`: per-line parse errors raised in this batch.
 * - `newKeys`: keys observed for the first time during this batch. Empty in
 *   steady state; non-empty triggers sink-side column extension.
 * - `firstLineNumber`: 1-based absolute line number of `lines.front()`.
 *
 * "Rows-only-empty" batches (`lines.empty()` with non-empty `errors`/`newKeys`)
 * are permitted. The parser always emits one final batch — possibly empty in
 * every field — before `OnFinished` so the sink sees a consistent terminal
 * state.
 */
struct StreamedBatch
{
    std::vector<LogLine> lines;
    std::vector<uint64_t> localLineOffsets;
    std::vector<std::string> errors;
    std::vector<std::string> newKeys;
    size_t firstLineNumber = 0;
};

/**
 * @brief Sink interface for the streaming log parser.
 *
 * Implementations receive structured progress events from the parser on a
 * worker thread and are responsible for marshalling those events to the
 * appropriate consumer (a Qt model, a buffering aggregator, etc.).
 *
 * Lifecycle:
 *   1. Exactly one `OnStarted()` at the beginning of a parse.
 *   2. Zero or more `OnBatch(StreamedBatch)` calls. The parser guarantees at
 *      least one final `OnBatch` (possibly empty in every field) before
 *      `OnFinished` so the sink can finalise its state under one contract.
 *   3. Exactly one `OnFinished(cancelled)` at the end, with
 *      `cancelled == true` if the parse was stopped via `stopToken`.
 *
 * Sink methods are not required to be thread-safe across themselves — they
 * are always called from a single serial-in-order worker.
 */
class StreamingLogSink
{
public:
    virtual ~StreamingLogSink() = default;

    /**
     * @brief Returns the canonical `KeyIndex` the parser interns keys into.
     *
     * The parser borrows this reference for the entire parse and uses it
     * from every worker via the thread-safe `GetOrInsert` / `KeyOf`
     * operations. The sink is the single source of truth for the dataset's
     * `KeyIndex`; the pipeline never owns a parallel copy. The reference
     * must remain stable between `OnStarted` and `OnFinished`.
     */
    virtual KeyIndex &Keys() = 0;

    /// Invoked once when the parser is about to start emitting batches.
    virtual void OnStarted() = 0;

    /// Invoked for each batch the parser emits. Move-out is encouraged; the
    /// parser does not retain any reference after the call returns.
    virtual void OnBatch(StreamedBatch batch) = 0;

    /// Invoked once when the parser is about to finish. `cancelled` is true
    /// if the parse was stopped via the `stopToken` cancellation mechanism.
    virtual void OnFinished(bool cancelled) = 0;

    /**
     * @brief Opt-in flag: skip the parser's coalescing accumulator and
     *        forward each pipeline batch directly to `OnBatch`.
     *
     * The default behaviour buffers parsed batches and flushes on a
     * line-count or wall-clock threshold so a GUI sink sees a smooth batch
     * rate. Sinks that immediately re-buffer the batch into their own
     * data structures (e.g. `BufferingSink`) should return `true` to skip
     * that wasted double-buffering. Sinks that drive a UI keep the default.
     */
    virtual bool PrefersUncoalesced() const
    {
        return false;
    }
};

} // namespace loglib
