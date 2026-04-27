#pragma once

#include <chrono>
#include <cstddef>

namespace loglib
{

/**
 * @brief Per-stage CPU- and wall-clock timing telemetry for one streaming
 *        parse call.
 *
 * Filled in when `internal::AdvancedParserOptions::timings != nullptr`. The
 * struct is copyable and trivially-default-constructible; the parser writes
 * every field exactly once, just before `OnFinished` fires on the sink.
 *
 * Stage A and Stage C run as `serial_in_order` filters, so their per-stage
 * CPU time equals their wall-clock contribution to the parse. Stage B is the
 * only parallel stage, so `stageBCpuTotal` is the sum across the
 * `enumerable_thread_specific` workers and may total up to
 * `(effectiveThreads * wallClockTotal)` on a perfectly-saturated parse.
 *
 * Reading the numbers:
 *   - Stage A wall-clock %  = `stageACpuTotal / wallClockTotal`
 *   - Stage B utilisation % = `stageBCpuTotal / (effectiveThreads * wallClockTotal)`
 *   - Stage C wall-clock %  = `stageCCpuTotal / wallClockTotal`
 *
 * The benchmark printer is responsible for the "% of wall clock" derivation;
 * the parser only writes the raw numerators and denominator. Reporting
 * `effectiveThreads` separately makes the struct interpretable by any
 * downstream consumer that did not invoke `ParseStreaming` itself.
 */
struct StageTimings
{
    std::chrono::nanoseconds wallClockTotal{0};
    std::chrono::nanoseconds stageACpuTotal{0};
    std::chrono::nanoseconds stageBCpuTotal{0};
    std::chrono::nanoseconds stageCCpuTotal{0};
    std::chrono::nanoseconds sinkTotal{0};
    unsigned int effectiveThreads = 1;
    size_t stageABatches = 0;
    size_t stageBBatches = 0;
    size_t stageCBatches = 0;
};

namespace internal
{

/**
 * @brief Tuning knobs for the streaming pipeline, behind the internal header.
 *
 * Reachable only via `<loglib/internal/parser_options.hpp>` so default callers
 * keep the two-field public `ParserOptions` surface. Default values reproduce
 * today's legacy behaviour bit-for-bit.
 */
struct AdvancedParserOptions
{
    /**
     * @brief Cap on per-process oneTBB parallelism. Picked so the streaming
     *        parser does not monopolise modern CPUs (e.g. 64-core boxes) when
     *        the caller does not provide an explicit thread count.
     */
    static constexpr unsigned int kDefaultMaxThreads = 8;

    /**
     * @brief Target byte size of one Stage A batch. Picked to keep each Stage
     *        B worker's working set warm in L2 while leaving room for ntokens
     *        in flight without ballooning resident memory.
     */
    static constexpr size_t kDefaultBatchSizeBytes = 1024 * 1024;

    /**
     * @brief Number of worker threads to drive Stage B with. `0` means "pick
     *        automatically": `min(hardware_concurrency, kDefaultMaxThreads)`.
     */
    unsigned int threads = 0;

    /**
     * @brief Stage A's batch byte target. Must be > 0; values smaller than a
     *        single line auto-expand to "next newline" to avoid splitting a
     *        line across batches.
     */
    size_t batchSizeBytes = kDefaultBatchSizeBytes;

    /**
     * @brief Pipeline depth in tokens. `0` defaults to `2 * effectiveThreads`
     *        which keeps Stage B busy without unbounded queuing.
     */
    size_t ntokens = 0;

    /**
     * @brief When true (default), each Stage B worker keeps a small
     *        thread-local key cache (interned key string -> KeyId) to avoid a
     *        round-trip through the canonical KeyIndex on every field.
     */
    bool useThreadLocalKeyCache = true;

    /**
     * @brief When true (default), each Stage B worker keeps a per-KeyId type
     *        cache (last-seen JSON / number type) to skip the simdjson
     *        `value.type()` call for fields with a stable type across the file.
     */
    bool useParseCache = true;

    /**
     * @brief When non-null, the pipeline populates this `StageTimings` with
     *        per-stage CPU time, wall-clock total, batch counts and
     *        `effectiveThreads`. The pointer is owned by the caller and only
     *        written once, immediately before `OnFinished` fires on the sink.
     *
     * `mutable` so the option can sit on a `const AdvancedParserOptions&`
     * captured by an enclosing `const` benchmark wrapper while still letting
     * the parser write the timings out. When null (default) the parser does
     * no timing work.
     */
    mutable StageTimings *timings = nullptr;
};

} // namespace internal
} // namespace loglib
