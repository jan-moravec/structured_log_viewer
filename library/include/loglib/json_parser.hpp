#pragma once

#include "key_index.hpp"
#include "log_parser.hpp"
#include "streaming_log_sink.hpp"

#include <simdjson.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <utility>
#include <vector>

namespace loglib
{

struct LogConfiguration;
class LogFile;

namespace detail
{

/**
 * @brief Opaque, .cpp-defined per-worker key string -> `KeyId` cache that
 *        backs PRD §4.1.
 *
 * Forward-declared here so the private static `JsonParser::ParseLine` can
 * name it by pointer in its parameter list without dragging the
 * `tsl::robin_map<std::string, KeyId, TransparentHash, TransparentEqual>`
 * machinery (and the transparent-hash adapter types) into the public
 * header. The full definition lives in `json_parser.cpp` next to the
 * Stage B `WorkerState`.
 *
 * Pointer-style is intentional: a null pointer is the "no cache" path and
 * is the same code path as `useThreadLocalKeyCache = false` — the legacy
 * behaviour kept for benchmarks and bisects (PRD req. 4.1.6 / 4.1.7).
 */
struct PerWorkerKeyCache;

} // namespace detail

/**
 * @brief Per-stage CPU- and wall-clock timing telemetry for one
 *        `JsonParser::ParseStreaming` call (PRD §4.7 / §6.1).
 *
 * Filled in when `JsonParserOptions::timings != nullptr`. The struct is
 * copyable and trivially-default-constructible; the parser overwrites every
 * field exactly once, just before `OnFinished` fires on the sink.
 *
 * Stage A and Stage C run as `serial_in_order` filters, so their per-stage
 * CPU time equals their wall-clock contribution to the parse. Stage B is
 * the only parallel stage, so `stageBCpuTotal` is the **sum** across the
 * `enumerable_thread_specific` workers and may total up to
 * `(effectiveThreads × wallClockTotal)` on a perfectly-saturated parse.
 *
 * Reading the numbers (PRD §4.7.2 / M4):
 *   - Stage A wall-clock %  = `stageACpuTotal / wallClockTotal`
 *   - Stage B utilisation % = `stageBCpuTotal / (effectiveThreads × wallClockTotal)`
 *   - Stage C wall-clock %  = `stageCCpuTotal / wallClockTotal`
 *
 * The benchmark printer (`benchmark_json.cpp`) is responsible for the
 * "% of wall clock" derivation; the parser only writes the raw numerators
 * and denominator. Reporting `effectiveThreads` separately makes the
 * struct interpretable by any downstream consumer that did not invoke
 * `ParseStreaming` itself.
 */
struct StageTimings
{
    /// Wall-clock duration of the entire `ParseStreaming` call (start to
    /// `OnFinished`). The denominator for any "% of wall clock" derivation.
    std::chrono::nanoseconds wallClockTotal{0};

    /// Sum of per-worker CPU time spent in Stage A. Stage A is
    /// `serial_in_order`, so this equals Stage A's wall-clock contribution.
    std::chrono::nanoseconds stageACpuTotal{0};

    /// Sum of per-worker CPU time spent in Stage B. Stage B is parallel;
    /// this can total up to `(effectiveThreads × wallClockTotal)` on a
    /// perfectly-saturated parse.
    std::chrono::nanoseconds stageBCpuTotal{0};

    /// Sum of per-worker CPU time spent in Stage C. Stage C is
    /// `serial_in_order`, so this equals Stage C's wall-clock contribution.
    std::chrono::nanoseconds stageCCpuTotal{0};

    /// Sink-side wall-clock cost. For the `BufferingSink` path this is
    /// folded into `stageCCpuTotal`; for the streaming sink it captures
    /// the queued-connection hop cost that lands on the GUI thread.
    std::chrono::nanoseconds sinkTotal{0};

    /// Number of TBB workers Stage B actually ran with. Needed by the
    /// printer to derive Stage B utilisation. Reported separately rather
    /// than re-derived so a downstream consumer can interpret the numbers
    /// without invoking the parser itself.
    unsigned int effectiveThreads = 1;

    /// Number of pipeline batches each stage processed (matched 1:1 between
    /// stages because the pipeline is `serial_in_order` end-to-end).
    size_t stageABatches = 0;
    size_t stageBBatches = 0;
    size_t stageCBatches = 0;
};

/**
 * @brief Options bundle for `JsonParser::ParseStreaming`.
 *
 * Tunes the parallel pipeline's worker count, batch granularity, scratch
 * caches and the optional configuration-driven Stage B passes (e.g.
 * timestamp promotion). All fields default to "production-good" values; the
 * defaults match the legacy single-shot `Parse(path)` behaviour as closely as
 * possible while still scaling to many threads on large files.
 *
 * See PRD req. 4.2.22 for the rationale behind each field.
 */
struct JsonParserOptions
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
    static constexpr size_t kDefaultBatchSizeBytes = 1024 * 1024; // 1 MiB

    /**
     * @brief Number of worker threads to drive Stage B with. `0` means "pick
     *        automatically": `std::min(hardware_concurrency, kDefaultMaxThreads)`.
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
     * @brief Optional log configuration for Stage B in-pipeline timestamp
     *        promotion. When non-null, Stage B parses any `Type::time` columns
     *        on the spot and `LogTable::Update` skips the redundant whole-data
     *        timestamp pass (PRD req. 4.2.21).
     */
    std::shared_ptr<const LogConfiguration> configuration;

    /**
     * @brief Cooperative cancellation. `JsonParser::ParseStreaming` polls this
     *        token in Stage A; when stop has been requested, the in-flight
     *        pipeline drains and `OnFinished(cancelled=true)` fires.
     */
    std::stop_token stopToken{};

    /**
     * @brief When true (default), each Stage B worker keeps a small
     *        thread-local key cache (interned key string -> KeyId) to avoid a
     *        round-trip through the canonical KeyIndex on every field. Off-by
     *        toggle is exposed for benchmarks and bisects (PRD req. 4.1.2/2b).
     */
    bool useThreadLocalKeyCache = true;

    /**
     * @brief When true (default), each Stage B worker keeps a per-KeyId type
     *        cache (last-seen JSON / number type) to skip the simdjson
     *        `value.type()` call for fields with a stable type across the
     *        file (PRD req. 4.1.15).
     */
    bool useParseCache = true;

    /**
     * @brief When non-null, Stage A / Stage B / Stage C populate this
     *        `StageTimings` instance with per-stage CPU time, wall-clock
     *        total, batch counts and `effectiveThreads`. The pointer is
     *        owned by the caller and only written once, immediately
     *        before `OnFinished` fires on the sink (PRD §4.7.1 / §6.1).
     *
     * `mutable` so the option can sit on a `const JsonParserOptions&`
     * captured by an enclosing `const` benchmark wrapper while still
     * letting the parser write the timings out. When null (default) the
     * parser does no timing work — there is zero allocation and zero
     * `steady_clock::now()` cost on the unused path (PRD §6.1).
     */
    mutable StageTimings *timings = nullptr;
};

/**
 * @class JsonParser
 * @brief A parser for processing log files in JSON format.
 *
 * This class extends the LogParser interface to provide functionality
 * for validating and parsing JSON log files.
 */
class JsonParser : public LogParser
{
public:
    /// Type alias kept for source-compat with legacy call sites.
    using Options = JsonParserOptions;

    /**
     * @brief PIMPL-style helper struct defined in `json_parser.cpp` that
     *        implements the streaming pipeline (`ParseStreaming`).
     *
     * Friended so it can reach the private nested `ParseCache` type and the
     * private static `ParseLine` helper without leaking those names into the
     * public header. The struct itself is *only* defined in the translation
     * unit — its incomplete declaration here is enough to satisfy the
     * `friend` lookup.
     */
    struct StreamingDetail;
    friend struct StreamingDetail;

    bool IsValid(const std::filesystem::path &file) const override;

    ParseResult Parse(const std::filesystem::path &file) const override;

    /**
     * @brief Synchronous parse with explicit `JsonParserOptions`.
     *
     * Equivalent to `ParseStreaming(file, BufferingSink, options)` followed by
     * `BufferingSink::TakeData/TakeErrors`. Provided so benchmarks and parity
     * tests can dial the streaming pipeline (thread count, batch size,
     * cache toggles) without having to construct a sink themselves.
     *
     * The default-options overload (the `LogParser`-virtual `Parse`) routes
     * through this implementation, so this is the one canonical synchronous
     * parse path (PRD req. 4.4.31).
     */
    ParseResult Parse(const std::filesystem::path &file, JsonParserOptions options) const;

    /**
     * @brief Streams the parse of @p file into @p sink using a multi-stage
     *        oneTBB pipeline.
     *
     * @param file       Already-opened (mmap'd) `LogFile` to read from. The
     *                   parser does not take ownership and does not mutate the
     *                   file's line-offset table — those updates are pushed
     *                   through the sink as `StreamedBatch::localLineOffsets`
     *                   so the consumer (e.g. `LogTable::AppendBatch`) can
     *                   apply them on its own thread.
     * @param sink       Sink that receives the parsed batches plus the
     *                   bracketing `OnStarted` / `OnFinished` lifecycle calls.
     *                   Sink methods are invoked from the parser's Stage C
     *                   (a `serial_in_order` filter), so they are always seen
     *                   on the same TBB worker, in order.
     * @param options    Tuning knobs; defaults match the legacy single-shot
     *                   `Parse(path)` behaviour as closely as possible.
     *
     * Pipeline shape (PRD req. 4.2.18):
     *   - Stage A (`serial_in_order`): chunks the mmap into ~`batchSizeBytes`
     *     batches at line boundaries.
     *   - Stage B (`parallel`): per-worker simdjson + KeyIndex interning;
     *     produces sorted-by-KeyId `LogLine`s and per-line errors.
     *   - Stage C (`serial_in_order`): stamps absolute line numbers,
     *     calculates the new-keys slice, then hands the resulting
     *     `StreamedBatch` to the sink.
     *
     * The pipeline cooperatively cancels via `opts.stopToken`; cancellation
     * latency is bounded by `ntokens × batchSizeBytes` of in-flight work.
     *
     * Empty files invoke `OnStarted` then `OnFinished(false)` and return
     * without ever calling `OnBatch`.
     */
    void ParseStreaming(LogFile &file, StreamingLogSink &sink, JsonParserOptions options = {}) const;

    std::string ToString(const LogLine &line) const override;

    /**
     * @brief Cold-path overload that serialises a `LogMap` directly.
     *
     * Useful for tests, debug dumps and other callers that do not have a
     * `LogLine`/`KeyIndex` pair on hand. The serialisation is byte-identical
     * to `ToString(LogLine)` for the same key/value pairs (PRD req. 4.1.14
     * parity contract).
     */
    std::string ToString(const LogMap &values) const;

private:
    /**
     * @brief Per-key type cache keyed by `KeyId`.
     *
     * Reshaped from the original `tsl::robin_map<std::string, ...>` so the hot
     * path indexes into a contiguous vector by KeyId rather than hashing the
     * key string per field. Lazily resized to `keyId + 1` (not
     * `KeyIndex::Size()`) to avoid a benign race with concurrent inserts in the
     * canonical KeyIndex once Stage B becomes parallel (PRD §4.1.15 / task 3.4).
     *
     * The bool "have-info" sentinel is kept as a plain `std::vector<bool>` for
     * simplicity; the parser is single-threaded for now and the per-worker
     * variant lands in task 4.0.
     */
    struct ParseCache
    {
        std::vector<simdjson::ondemand::json_type> keyTypes;
        std::vector<simdjson::ondemand::number_type> numberTypes;
        std::vector<bool> hasKeyType;
        std::vector<bool> hasNumberType;

        void EnsureCapacity(KeyId id)
        {
            const size_t needed = static_cast<size_t>(id) + 1;
            if (keyTypes.size() < needed)
            {
                keyTypes.resize(needed, simdjson::ondemand::json_type::null);
                hasKeyType.resize(needed, false);
            }
            if (numberTypes.size() < needed)
            {
                numberTypes.resize(needed, simdjson::ondemand::number_type::signed_integer);
                hasNumberType.resize(needed, false);
            }
        }
    };

    /**
     * @brief Parses one JSON object into a sorted-by-`KeyId` vector of
     *        (KeyId, LogValue) pairs.
     *
     * @param object         simdjson object iterator for the line.
     * @param keys           Canonical KeyIndex; new keys observed during
     *                       parsing are inserted via `GetOrInsert`.
     * @param cache          Per-key type cache, indexed by `KeyId`.
     * @param sourceIsStable When true, raw bytes returned by simdjson outlive
     *                       the resulting `LogLine` (i.e. simdjson is iterating
     *                       directly over the mmap), so string fields can be
     *                       emitted as `std::string_view`s pointing into that
     *                       backing storage. When false, string and raw-JSON
     *                       fields must be materialised as owned `std::string`
     *                       to detach from the per-line scratch buffer. See
     *                       PRD req. 4.1.6 / 4.1.15a.
     * @param keyCache       Optional per-worker key string -> `KeyId` cache
     *                       (PRD §4.1). May be `nullptr` to skip the cache
     *                       entirely; pass-through to `KeyIndex::GetOrInsert`
     *                       on every field key in that case. When non-null,
     *                       fast-path keys (`isView == true`) check the cache
     *                       first and only consult the canonical `KeyIndex`
     *                       on a miss.
     * @param useKeyCache    Mirrors `JsonParserOptions::useThreadLocalKeyCache`;
     *                       a `false` value forces the no-cache path even when
     *                       `keyCache != nullptr`. The two parameters are kept
     *                       distinct so callers do not have to nullable-thread
     *                       the cache pointer through every call site.
     * @return The parsed pairs in ascending KeyId order, ready to feed
     *         `LogLine`'s pre-sorted constructor.
     */
    static std::vector<std::pair<KeyId, LogValue>> ParseLine(
        simdjson::ondemand::object &object,
        KeyIndex &keys,
        ParseCache &cache,
        bool sourceIsStable,
        detail::PerWorkerKeyCache *keyCache = nullptr,
        bool useKeyCache = false
    );
};

} // namespace loglib
