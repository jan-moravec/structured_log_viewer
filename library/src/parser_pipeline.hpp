#pragma once

#include "timestamp_promotion.hpp"

#include "loglib/json_parser.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_configuration.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_processing.hpp"
#include "loglib/streaming_log_sink.hpp"

#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/parallel_pipeline.h>

#include <tsl/robin_map.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace loglib::detail
{

/// Transparent hash adapter for the per-worker key cache; routes every overload through
/// `std::hash<std::string_view>` so a `string_view` lookup hits the same bucket the
/// matching `std::string` was stored under.
struct TransparentStringHash
{
    using is_transparent = void;

    size_t operator()(std::string_view sv) const noexcept
    {
        return std::hash<std::string_view>{}(sv);
    }
    size_t operator()(const std::string &s) const noexcept
    {
        return std::hash<std::string_view>{}(s);
    }
    size_t operator()(const char *s) const noexcept
    {
        return std::hash<std::string_view>{}(std::string_view(s));
    }
};

struct TransparentStringEqual
{
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }
    bool operator()(std::string_view lhs, const std::string &rhs) const noexcept
    {
        return lhs == rhs;
    }
    bool operator()(const std::string &lhs, std::string_view rhs) const noexcept
    {
        return lhs == rhs;
    }
    bool operator()(const std::string &lhs, const std::string &rhs) const noexcept
    {
        return lhs == rhs;
    }
};

/// Per-worker key string -> KeyId cache. On a hit, `find(string_view)` allocates nothing;
/// on a miss the worker calls `KeyIndex::GetOrInsert` once, then writes back.
struct PerWorkerKeyCache
{
    tsl::robin_map<std::string, KeyId, TransparentStringHash, TransparentStringEqual> map;
};

/// Per-worker scratch shared across every parser. Holds the key cache plus the
/// per-time-column carry-over caches the post-decoding timestamp hook needs.
struct WorkerScratchBase
{
    PerWorkerKeyCache keyCache;
    std::vector<std::optional<LastValidTimestampParse>> lastValidTimestamps;
    TimestampParseScratch tsScratch;
    std::vector<LastTimestampBytesHit> lastBytesHits;

    void EnsureTimeColumnCapacity(size_t n)
    {
        if (lastValidTimestamps.size() < n)
        {
            lastValidTimestamps.resize(n);
        }
        if (lastBytesHits.size() < n)
        {
            lastBytesHits.resize(n);
        }
    }

    /// Convenience inline hook the parser calls right after pushing a fresh `LogLine`
    /// onto its `ParsedPipelineBatch`. Forwards to the shared `PromoteLineTimestamps`
    /// helper, threading the worker's per-column carry-over caches through. Doing it
    /// inline (rather than as a post-decode pass over the parsed batch) keeps the
    /// line's freshly-written values hot in L1 and avoids a second walk over the
    /// per-line `(KeyId, LogValue)` vector.
    void PromoteTimestamps(class LogLine &line, std::span<const TimeColumnSpec> timeColumns);
};

/// Templated wrapper that lets a parser bolt format-specific scratch (e.g. a
/// `simdjson::ondemand::parser` instance + padded line buffer) onto the shared
/// `WorkerScratchBase`.
template <class UserState>
struct WorkerScratch : WorkerScratchBase
{
    UserState user;
};

/// Routes a per-field key lookup through the per-worker cache when the option is enabled
/// and the input is a `string_view` whose bytes outlive the line. On the cache-miss path
/// we call `KeyIndex::GetOrInsert(view)` exactly once and write the new mapping back
/// into the local cache.
inline KeyId InternKeyVia(std::string_view key, KeyIndex &keys, PerWorkerKeyCache *cache, bool useCache)
{
    if (!useCache || cache == nullptr)
    {
        return keys.GetOrInsert(key);
    }
    if (auto it = cache->map.find(key); it != cache->map.end())
    {
        return it->second;
    }
    const KeyId id = keys.GetOrInsert(key);
    cache->map.emplace(std::string(key), id);
    return id;
}

/// Parsed Stage B output. Stage B (parallel) emits one of these per Stage A token;
/// Stage C (serial_in_order) re-asserts ordering before the sink sees them.
struct ParsedPipelineBatch
{
    uint64_t batchIndex = 0;
    std::vector<LogLine> lines;
    std::vector<uint64_t> localLineOffsets;
    std::vector<std::string> errors;
    /// Number of source lines this batch consumed (parsed lines + parse errors + skipped
    /// empty lines). Stage C uses this exact value to advance its running line-number
    /// cursor across batches, so it must count every input-advancing iteration.
    size_t totalLineCount = 0;
};

inline void
WorkerScratchBase::PromoteTimestamps(LogLine &line, std::span<const TimeColumnSpec> timeColumns)
{
    if (timeColumns.empty())
    {
        return;
    }
    PromoteLineTimestamps(line, timeColumns, lastValidTimestamps, lastBytesHits, tsScratch);
}

/// Inputs the harness needs from the parser's `ParseStreaming` call. Mirrors today's
/// `JsonParserOptions` defaults bit-for-bit so a default-constructed instance reproduces
/// the legacy synchronous `Parse(path)` behaviour.
struct PipelineHarnessOptions
{
    static constexpr unsigned int kDefaultMaxThreads = 8;
    static constexpr size_t kDefaultBatchSizeBytes = 1024 * 1024;

    unsigned int threads = 0;
    size_t batchSizeBytes = kDefaultBatchSizeBytes;
    size_t ntokens = 0;
    std::shared_ptr<const LogConfiguration> configuration;
    std::stop_token stopToken{};
    StageTimings *timings = nullptr;
};

/// Resolves `effectiveThreads` and `ntokens` against today's defaulting rules:
/// `threads == 0 -> min(hardware_concurrency, kDefaultMaxThreads)`, `ntokens == 0 ->
/// 2 * effectiveThreads`. Both are clamped to a minimum of 1.
struct ResolvedPipelineSettings
{
    unsigned int effectiveThreads = 1;
    size_t ntokens = 0;
    size_t batchSizeBytes = PipelineHarnessOptions::kDefaultBatchSizeBytes;
};

ResolvedPipelineSettings ResolvePipelineSettings(const PipelineHarnessOptions &opts);

namespace pipeline_detail
{

constexpr size_t kStreamFlushLines = 1000;
constexpr auto kStreamFlushInterval = std::chrono::milliseconds(50);

}  // namespace pipeline_detail

/// Templated entry point. The parser supplies a Stage A token type (`Token`), a
/// per-worker scratch slot (`UserState`), and two callables:
///
///   - `stageADriver(Token& out) -> bool` — serial_in_order; returns false at EOF.
///     The harness wraps it with the `stop_token` poll and Stage A telemetry.
///   - `stageBDecoder(Token, WorkerScratch<UserState>&, KeyIndex&,
///                    std::span<const TimeColumnSpec>, ParsedPipelineBatch&)` — parallel.
///
/// The harness owns: sink lifecycle (`OnStarted` / `OnFinished`), Stage C coalescing,
/// new-keys diff, per-worker key cache, per-line timestamp promotion (applied on every
/// line in the parsed batch before Stage C sees it), telemetry, and cooperative
/// cancellation.
template <class Token, class UserState, class StageADriver, class StageBDecoder>
void RunParserPipeline(
    LogFile &file,
    StreamingLogSink &sink,
    const PipelineHarnessOptions &opts,
    StageADriver &&stageADriver,
    StageBDecoder &&stageBDecoder
)
{
    sink.OnStarted();

    if (opts.stopToken.stop_requested())
    {
        sink.OnFinished(true);
        return;
    }

    if (file.Size() == 0 || file.Data() == nullptr)
    {
        sink.OnFinished(false);
        return;
    }

    const ResolvedPipelineSettings settings = ResolvePipelineSettings(opts);
    file.ReserveLineOffsets(file.Size() / 100);

    KeyIndex &keys = sink.Keys();

    const std::vector<TimeColumnSpec> timeColumns =
        BuildTimeColumnSpecs(keys, opts.configuration.get());

    oneapi::tbb::enumerable_thread_specific<WorkerScratch<UserState>> workers;

    StageTimings *timingsOut = opts.timings;
    const auto wallClockStart = std::chrono::steady_clock::now();
    std::chrono::nanoseconds stageACpuTotal{0};
    size_t stageABatches = 0;
    oneapi::tbb::enumerable_thread_specific<std::chrono::nanoseconds> stageBCpuPerWorker(
        std::chrono::nanoseconds{0}
    );
    std::atomic<size_t> stageBBatches{0};
    std::chrono::nanoseconds stageCCpuTotal{0};
    size_t stageCBatches = 0;

    const bool prefersUncoalesced = sink.PrefersUncoalesced();
    StreamedBatch pending;
    bool pendingPrimed = false;
    size_t prevKeyCount = keys.Size();
    auto lastFlush = std::chrono::steady_clock::now();
    size_t nextLineNumber = 1;

    const std::stop_token stopToken = opts.stopToken;
    std::span<const TimeColumnSpec> timeColumnsSpan(timeColumns);

    auto stageA = [&](oneapi::tbb::flow_control &fc) -> Token {
        if (stopToken.stop_requested())
        {
            fc.stop();
            return Token{};
        }
        const auto stageStart = std::chrono::steady_clock::now();
        Token token{};
        const bool produced = stageADriver(token);
        if (!produced)
        {
            fc.stop();
            return Token{};
        }
        stageACpuTotal += std::chrono::steady_clock::now() - stageStart;
        ++stageABatches;
        return token;
    };

    auto stageB = [&](Token token) -> ParsedPipelineBatch {
        const auto stageStart = std::chrono::steady_clock::now();
        WorkerScratch<UserState> &worker = workers.local();
        worker.EnsureTimeColumnCapacity(timeColumnsSpan.size());

        ParsedPipelineBatch parsed;
        stageBDecoder(std::move(token), worker, keys, timeColumnsSpan, parsed);

        stageBCpuPerWorker.local() += std::chrono::steady_clock::now() - stageStart;
        stageBBatches.fetch_add(1, std::memory_order_relaxed);
        return parsed;
    };

    // Stage C lives inline (rather than in a helper) so the compiler can keep `pending`,
    // `pendingPrimed`, `prevKeyCount`, `lastFlush` and `nextLineNumber` in registers across
    // the per-batch body. Pulling them through a struct of pointers shows up as a measurable
    // throughput regression on `[large]` (hot serial_in_order path, ~173 calls/parse).
    auto emitNewKeysInto = [&](StreamedBatch &out) {
        const size_t currentKeyCount = keys.Size();
        if (currentKeyCount > prevKeyCount)
        {
            out.newKeys.reserve(out.newKeys.size() + (currentKeyCount - prevKeyCount));
            for (size_t i = prevKeyCount; i < currentKeyCount; ++i)
            {
                out.newKeys.emplace_back(std::string(keys.KeyOf(static_cast<KeyId>(i))));
            }
            prevKeyCount = currentKeyCount;
        }
    };

    auto flushPending = [&](bool force) {
        if (!force && pending.lines.size() < pipeline_detail::kStreamFlushLines &&
            (std::chrono::steady_clock::now() - lastFlush) < pipeline_detail::kStreamFlushInterval)
        {
            return;
        }
        emitNewKeysInto(pending);
        sink.OnBatch(std::move(pending));
        pending = StreamedBatch{};
        pendingPrimed = false;
        lastFlush = std::chrono::steady_clock::now();
    };

    auto stageC = [&](ParsedPipelineBatch parsed) {
        const auto stageStart = std::chrono::steady_clock::now();

        const size_t lineNumberDelta = nextLineNumber - 1;
        if (lineNumberDelta != 0)
        {
            for (LogLine &line : parsed.lines)
            {
                line.FileReference().ShiftLineNumber(lineNumberDelta);
            }
        }

        if (prefersUncoalesced)
        {
            StreamedBatch out;
            out.lines = std::move(parsed.lines);
            out.localLineOffsets = std::move(parsed.localLineOffsets);
            out.errors = std::move(parsed.errors);
            out.firstLineNumber = nextLineNumber;
            emitNewKeysInto(out);

            nextLineNumber += parsed.totalLineCount;

            sink.OnBatch(std::move(out));
            stageCCpuTotal += std::chrono::steady_clock::now() - stageStart;
            ++stageCBatches;
            return;
        }

        if (!pendingPrimed)
        {
            pending.firstLineNumber = nextLineNumber;
            pendingPrimed = true;
        }
        if (!parsed.lines.empty())
        {
            pending.lines.insert(
                pending.lines.end(),
                std::make_move_iterator(parsed.lines.begin()),
                std::make_move_iterator(parsed.lines.end())
            );
        }
        if (!parsed.localLineOffsets.empty())
        {
            pending.localLineOffsets.insert(
                pending.localLineOffsets.end(),
                std::make_move_iterator(parsed.localLineOffsets.begin()),
                std::make_move_iterator(parsed.localLineOffsets.end())
            );
        }
        if (!parsed.errors.empty())
        {
            pending.errors.insert(
                pending.errors.end(),
                std::make_move_iterator(parsed.errors.begin()),
                std::make_move_iterator(parsed.errors.end())
            );
        }

        nextLineNumber += parsed.totalLineCount;

        flushPending(false);

        stageCCpuTotal += std::chrono::steady_clock::now() - stageStart;
        ++stageCBatches;
    };

    oneapi::tbb::global_control gc(
        oneapi::tbb::global_control::max_allowed_parallelism,
        static_cast<size_t>(settings.effectiveThreads)
    );

    oneapi::tbb::parallel_pipeline(
        settings.ntokens,
        oneapi::tbb::make_filter<void, Token>(oneapi::tbb::filter_mode::serial_in_order, stageA) &
            oneapi::tbb::make_filter<Token, ParsedPipelineBatch>(oneapi::tbb::filter_mode::parallel, stageB) &
            oneapi::tbb::make_filter<ParsedPipelineBatch, void>(oneapi::tbb::filter_mode::serial_in_order, stageC)
    );

    if (prefersUncoalesced)
    {
        StreamedBatch tail;
        tail.firstLineNumber = nextLineNumber;
        emitNewKeysInto(tail);
        sink.OnBatch(std::move(tail));
    }
    else if (pendingPrimed || keys.Size() > prevKeyCount)
    {
        if (!pendingPrimed)
        {
            pending.firstLineNumber = nextLineNumber;
            pendingPrimed = true;
        }
        flushPending(true);
    }
    else
    {
        StreamedBatch tail;
        tail.firstLineNumber = nextLineNumber;
        sink.OnBatch(std::move(tail));
    }

    if (timingsOut != nullptr)
    {
        timingsOut->wallClockTotal = std::chrono::steady_clock::now() - wallClockStart;
        timingsOut->stageACpuTotal = stageACpuTotal;
        timingsOut->stageBCpuTotal = std::chrono::nanoseconds{0};
        for (const std::chrono::nanoseconds &perWorker : stageBCpuPerWorker)
        {
            timingsOut->stageBCpuTotal += perWorker;
        }
        timingsOut->stageCCpuTotal = stageCCpuTotal;
        timingsOut->sinkTotal = std::chrono::nanoseconds{0};
        timingsOut->effectiveThreads = settings.effectiveThreads;
        timingsOut->stageABatches = stageABatches;
        timingsOut->stageBBatches = stageBBatches.load(std::memory_order_relaxed);
        timingsOut->stageCBatches = stageCBatches;
    }

    sink.OnFinished(stopToken.stop_requested());
}

} // namespace loglib::detail
