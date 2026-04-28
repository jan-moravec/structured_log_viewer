#pragma once

#include "timestamp_promotion.hpp"
#include "transparent_string_hash.hpp"

#include "loglib/internal/parser_options.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_configuration.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_processing.hpp"
#include "loglib/parser_options.hpp"
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

/// Per-worker key string -> KeyId cache. Hit: `find(string_view)` is alloc-free;
/// miss: one `KeyIndex::GetOrInsert` plus a write-back.
struct PerWorkerKeyCache
{
    tsl::robin_map<std::string, KeyId, TransparentStringHash, TransparentStringEqual> map;
};

/// Per-worker scratch shared across parsers: key cache plus per-time-column
/// carry-over for the inline timestamp-promotion hook.
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

    /// Called inline after pushing a `LogLine` onto its `ParsedPipelineBatch`,
    /// while the freshly-written values are still hot in L1.
    void PromoteTimestamps(class LogLine &line, std::span<const TimeColumnSpec> timeColumns);
};

/// Bolts format-specific scratch (e.g. simdjson parser + padded buffer) onto
/// the shared base.
template <class UserState> struct WorkerScratch : WorkerScratchBase
{
    UserState user;
};

/// Routes a key lookup through the per-worker cache when enabled. The view's
/// bytes must outlive the cache entry on the miss path.
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

/// Parsed Stage B output. Stage C re-asserts ordering before the sink sees it.
struct ParsedPipelineBatch
{
    uint64_t batchIndex = 0;
    std::vector<LogLine> lines;
    std::vector<uint64_t> localLineOffsets;
    std::vector<std::string> errors;
    /// Source lines consumed (parsed + errors + skipped empties). Stage C uses
    /// this to advance its running line-number cursor across batches.
    size_t totalLineCount = 0;
};

inline void WorkerScratchBase::PromoteTimestamps(LogLine &line, std::span<const TimeColumnSpec> timeColumns)
{
    if (timeColumns.empty())
    {
        return;
    }
    PromoteLineTimestamps(line, timeColumns, lastValidTimestamps, lastBytesHits, tsScratch);
}

/// Resolved defaults for `effectiveThreads` and `ntokens`. Both >= 1.
struct ResolvedPipelineSettings
{
    unsigned int effectiveThreads = 1;
    size_t ntokens = 0;
};

ResolvedPipelineSettings ResolvePipelineSettings(const internal::AdvancedParserOptions &advanced);

namespace pipeline_detail
{

constexpr size_t kStreamFlushLines = 1000;
constexpr auto kStreamFlushInterval = std::chrono::milliseconds(50);

} // namespace pipeline_detail

/// Streaming-pipeline entry point. Stage A `stageADriver(Token&) -> bool` is
/// serial_in_order; Stage B `stageBDecoder(Token, scratch, keys, columns,
/// out)` is parallel. Stage C coalescing, new-keys diff, inline timestamp
/// promotion, telemetry, and stop_token cancellation are owned by the harness.
template <class Token, class UserState, class StageADriver, class StageBDecoder>
void RunParserPipeline(
    LogFile &file,
    StreamingLogSink &sink,
    const ParserOptions &options,
    const internal::AdvancedParserOptions &advanced,
    StageADriver &&stageADriver,
    StageBDecoder &&stageBDecoder
)
{
    sink.OnStarted();

    if (options.stopToken.stop_requested())
    {
        sink.OnFinished(true);
        return;
    }

    if (file.Size() == 0 || file.Data() == nullptr)
    {
        sink.OnFinished(false);
        return;
    }

    const ResolvedPipelineSettings settings = ResolvePipelineSettings(advanced);
    file.ReserveLineOffsets(file.Size() / 100);

    KeyIndex &keys = sink.Keys();

    const std::vector<TimeColumnSpec> timeColumns = BuildTimeColumnSpecs(keys, options.configuration.get());

    oneapi::tbb::enumerable_thread_specific<WorkerScratch<UserState>> workers;

    StageTimings *timingsOut = advanced.timings;
    const bool collectTimings = (timingsOut != nullptr);
    const auto wallClockStart =
        collectTimings ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    std::chrono::nanoseconds stageACpuTotal{0};
    size_t stageABatches = 0;
    oneapi::tbb::enumerable_thread_specific<std::chrono::nanoseconds> stageBCpuPerWorker(std::chrono::nanoseconds{0});
    std::atomic<size_t> stageBBatches{0};
    std::chrono::nanoseconds stageCCpuTotal{0};
    size_t stageCBatches = 0;

    const bool prefersUncoalesced = sink.PrefersUncoalesced();
    StreamedBatch pending;
    bool pendingPrimed = false;
    size_t prevKeyCount = keys.Size();
    auto lastFlush = std::chrono::steady_clock::now();
    size_t nextLineNumber = 1;

    const std::stop_token stopToken = options.stopToken;
    std::span<const TimeColumnSpec> timeColumnsSpan(timeColumns);

    auto stageA = [&](oneapi::tbb::flow_control &fc) -> Token {
        if (stopToken.stop_requested())
        {
            fc.stop();
            return Token{};
        }
        const auto stageStart =
            collectTimings ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        Token token{};
        const bool produced = stageADriver(token);
        if (!produced)
        {
            fc.stop();
            return Token{};
        }
        if (collectTimings)
        {
            stageACpuTotal += std::chrono::steady_clock::now() - stageStart;
            ++stageABatches;
        }
        return token;
    };

    auto stageB = [&](Token token) -> ParsedPipelineBatch {
        const auto stageStart =
            collectTimings ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        WorkerScratch<UserState> &worker = workers.local();
        worker.EnsureTimeColumnCapacity(timeColumnsSpan.size());

        ParsedPipelineBatch parsed;
        stageBDecoder(std::move(token), worker, keys, timeColumnsSpan, parsed);

        if (collectTimings)
        {
            stageBCpuPerWorker.local() += std::chrono::steady_clock::now() - stageStart;
            stageBBatches.fetch_add(1, std::memory_order_relaxed);
        }
        return parsed;
    };

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
        const auto stageStart =
            collectTimings ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

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
            if (collectTimings)
            {
                stageCCpuTotal += std::chrono::steady_clock::now() - stageStart;
                ++stageCBatches;
            }
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

        if (collectTimings)
        {
            stageCCpuTotal += std::chrono::steady_clock::now() - stageStart;
            ++stageCBatches;
        }
    };

    oneapi::tbb::global_control gc(
        oneapi::tbb::global_control::max_allowed_parallelism, static_cast<size_t>(settings.effectiveThreads)
    );

    oneapi::tbb::parallel_pipeline(
        settings.ntokens,
        // A: serial_in_order — drives Stage A tokens in source order.
        oneapi::tbb::make_filter<void, Token>(oneapi::tbb::filter_mode::serial_in_order, stageA) &
            // B: parallel — decode batches concurrently.
            oneapi::tbb::make_filter<Token, ParsedPipelineBatch>(oneapi::tbb::filter_mode::parallel, stageB) &
            // C: serial_in_order — coalesce / emit batches in source order.
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
