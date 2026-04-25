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
 * @brief One unit of work handed from the streaming parser's tail (Stage C) to
 *        an installed `StreamingLogSink`.
 *
 * A batch is the parser's only externally observable accumulation point; the
 * sink learns about new lines, new keys (i.e. new columns) and parse errors
 * exclusively through these objects.
 *
 * Field semantics (per PRD req. 4.3.25):
 * - `lines`: parsed `LogLine`s for this batch, already bound to the canonical
 *   `KeyIndex`. Move-out on receipt; the parser will not touch them again.
 * - `localLineOffsets`: byte offsets of each line within the source file, in
 *   absolute terms. Indexed in the same order as `lines`. Used by
 *   `LogTable::AppendBatch` to extend the per-line offset table on the owning
 *   `LogFile`.
 * - `errors`: per-line parse errors raised in Stage B during this batch. The
 *   sink is the place that surfaces these; the parser does not log them
 *   anywhere else.
 * - `newKeys`: keys observed for the first time during this batch (Stage C
 *   computes the `[prevKeyCount, currentKeyCount)` slice once per batch). May
 *   be empty in steady state; non-empty triggers column extension on the
 *   sink-side data model.
 * - `firstLineNumber`: 1-based absolute line number of `lines.front()` in the
 *   source file. Allows the sink to compute the row range affected by this
 *   batch without recomputing line numbers.
 *
 * A "rows-only-empty" batch (`lines.empty() && (!errors.empty() ||
 * !newKeys.empty())`) is permitted: the sink must not crash on it. The parser
 * always emits one final batch (potentially empty in every field) right before
 * `OnFinished` so the sink sees a consistent terminal state.
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
 * @brief Sink interface for the streaming JSON parser.
 *
 * Implementations receive structured progress events from
 * `JsonParser::ParseStreaming` on a worker thread (the parser's Stage C
 * filter) and are responsible for marshalling those events to the appropriate
 * consumer (a Qt model, a buffering aggregator, a CLI progress bar, etc.).
 *
 * Lifecycle (PRD req. 4.3.25, 4.3.26a):
 *   1. Exactly one `OnStarted()` at the beginning of a parse.
 *   2. Zero or more `OnBatch(StreamedBatch)` calls. The parser guarantees at
 *      least one final `OnBatch` is delivered before `OnFinished`, even if its
 *      `lines`/`errors`/`newKeys` are all empty, so the sink can finalise its
 *      state under a single contract.
 *   3. Exactly one `OnFinished(cancelled)` at the end of the parse, where
 *      `cancelled == true` if the parse was stopped via the
 *      `JsonParserOptions::stopToken` mechanism.
 *
 * The sink methods are not required to be thread-safe across themselves — the
 * parser's Stage C is a `serial_in_order` filter, so they are always called
 * from the same TBB worker, in order.
 */
class StreamingLogSink
{
public:
    virtual ~StreamingLogSink() = default;

    /**
     * @brief Returns the canonical `KeyIndex` the parser must intern keys into.
     *
     * The parser borrows this reference for the entire duration of
     * `ParseStreaming` and uses it from every Stage B worker via the
     * thread-safe `GetOrInsert` / `KeyOf` operations (PRD req. 4.1.2/2a).
     * The sink is therefore the single source of truth for the dataset's
     * `KeyIndex`; the streaming pipeline never owns a parallel copy.
     *
     * Implementations must return a stable reference for the duration of the
     * parse — i.e. the underlying `KeyIndex` object must not be moved or
     * destroyed between `OnStarted` and `OnFinished`.
     */
    virtual KeyIndex &Keys() = 0;

    /**
     * @brief Invoked once when the parser is about to start emitting batches.
     */
    virtual void OnStarted() = 0;

    /**
     * @brief Invoked for each batch emitted by the parser. Move-out of the
     *        batch is encouraged; the parser does not retain any reference to
     *        the batch after this call returns.
     */
    virtual void OnBatch(StreamedBatch batch) = 0;

    /**
     * @brief Invoked once when the parser is about to finish.
     *
     * @param cancelled True if the parse was stopped via the `stopToken`
     *                  cooperative cancellation mechanism. False on normal
     *                  end-of-file completion (including the file-empty case).
     */
    virtual void OnFinished(bool cancelled) = 0;
};

} // namespace loglib
