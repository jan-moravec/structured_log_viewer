#pragma once

#include "loglib/internal/parser_options.hpp"
#include "loglib/internal/timestamp_promotion.hpp"
#include "loglib/internal/transparent_string_hash.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_configuration.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_processing.hpp"
#include "loglib/parser_options.hpp"
#include "loglib/stop_token.hpp"
#include "loglib/streaming_log_sink.hpp"

#include <fmt/format.h>

#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/parallel_pipeline.h>

#include <tsl/robin_map.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
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

/// Routes a key lookup through the per-worker cache. The view's bytes must
/// outlive the cache entry on the miss path. Passing `cache == nullptr`
/// falls back to a direct `KeyIndex::GetOrInsert` call.
inline KeyId InternKeyVia(std::string_view key, KeyIndex &keys, PerWorkerKeyCache *cache)
{
    if (cache == nullptr)
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

/// Per-line error emitted by Stage B. `relativeLine` is the 1-based line
/// number within the source batch (i.e. it resets to 1 at every Stage B
/// invocation). Stage C composes the absolute `Error on line N: <body>`
/// string by adding its running line-number cursor — Stage B cannot do this
/// itself because it has no view of the cumulative line count.
struct ParsedLineError
{
    size_t relativeLine = 0;
    std::string body;
};

/// Parsed Stage B output. Stage C re-asserts ordering before the sink sees it.
struct ParsedPipelineBatch
{
    uint64_t batchIndex = 0;
    std::vector<LogLine> lines;
    std::vector<uint64_t> localLineOffsets;
    std::vector<ParsedLineError> errors;
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
/// promotion, and stop_token cancellation are owned by the harness.
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

    // Both early-exit paths still need to honour the
    // `StreamingLogSink` contract: at least one `OnBatch` must precede
    // `OnFinished`. Emit a rows-empty batch with the canonical
    // `firstLineNumber = 1` so sinks that lazily initialise on the first
    // `OnBatch` (the contract the docstring promises) work uniformly with
    // the streaming path.
    if (options.stopToken.stop_requested())
    {
        StreamedBatch tail;
        tail.firstLineNumber = 1;
        sink.OnBatch(std::move(tail));
        sink.OnFinished(true);
        return;
    }

    if (file.Size() == 0 || file.Data() == nullptr)
    {
        StreamedBatch tail;
        tail.firstLineNumber = 1;
        sink.OnBatch(std::move(tail));
        sink.OnFinished(false);
        return;
    }

    const ResolvedPipelineSettings settings = ResolvePipelineSettings(advanced);
    file.ReserveLineOffsets(file.Size() / 100);

    KeyIndex &keys = sink.Keys();

    const std::vector<TimeColumnSpec> timeColumns = BuildTimeColumnSpecs(keys, options.configuration.get());

    oneapi::tbb::enumerable_thread_specific<WorkerScratch<UserState>> workers;

    const bool prefersUncoalesced = sink.PrefersUncoalesced();
    StreamedBatch pending;
    bool pendingPrimed = false;
    size_t prevKeyCount = keys.Size();
    auto lastFlush = std::chrono::steady_clock::now();
    size_t nextLineNumber = 1;

    const StopToken stopToken = options.stopToken;
    std::span<const TimeColumnSpec> timeColumnsSpan(timeColumns);

    auto stageA = [&](oneapi::tbb::flow_control &fc) -> Token {
        if (stopToken.stop_requested())
        {
            fc.stop();
            return Token{};
        }
        Token token{};
        const bool produced = stageADriver(token);
        if (!produced)
        {
            fc.stop();
            return Token{};
        }
        return token;
    };

    auto stageB = [&](Token token) -> ParsedPipelineBatch {
        WorkerScratch<UserState> &worker = workers.local();
        worker.EnsureTimeColumnCapacity(timeColumnsSpan.size());

        ParsedPipelineBatch parsed;
        stageBDecoder(std::move(token), worker, keys, timeColumnsSpan, parsed);

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
        const size_t lineNumberDelta = nextLineNumber - 1;
        if (lineNumberDelta != 0)
        {
            for (LogLine &line : parsed.lines)
            {
                line.FileReference().ShiftLineNumber(lineNumberDelta);
            }
        }

        // Stage B's `relativeLine` is 1-based within the batch. Compose the
        // absolute "Error on line N: ..." message here so the line-number
        // shift on `LogLine::FileReference` and the line-number shown to the
        // user stay in lockstep across batches.
        auto formatErrorsInto = [&](std::vector<std::string> &out) {
            if (parsed.errors.empty())
            {
                return;
            }
            out.reserve(out.size() + parsed.errors.size());
            for (auto &err : parsed.errors)
            {
                out.push_back(
                    fmt::format("Error on line {}: {}", err.relativeLine + lineNumberDelta, std::move(err.body))
                );
            }
            parsed.errors.clear();
        };

        if (prefersUncoalesced)
        {
            StreamedBatch out;
            out.lines = std::move(parsed.lines);
            out.localLineOffsets = std::move(parsed.localLineOffsets);
            formatErrorsInto(out.errors);
            out.firstLineNumber = nextLineNumber;
            emitNewKeysInto(out);

            nextLineNumber += parsed.totalLineCount;

            sink.OnBatch(std::move(out));
            return;
        }

        // Defer the prime until we have a real line: `firstLineNumber` is
        // documented (and treated by sinks) as the 1-based absolute line
        // number of the first source line in the batch — *not* of the first
        // chunk that hit `pending`. Priming on an all-error / all-blank chunk
        // would set `firstLineNumber` to a value strictly less than
        // `lines.front().FileReference().GetLineNumber()` once the next
        // chunk's first real line lands here.
        if (!parsed.lines.empty())
        {
            if (!pendingPrimed)
            {
                pending.firstLineNumber = nextLineNumber;
                pendingPrimed = true;
            }
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
        formatErrorsInto(pending.errors);

        nextLineNumber += parsed.totalLineCount;

        flushPending(false);
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
    else
    {
        // `pendingPrimed` flips on the first non-empty `parsed.lines`, but
        // `pending.errors` and `pending.localLineOffsets` may have
        // accumulated from earlier all-error / all-blank chunks. Drain them
        // here so they don't silently disappear when the parser ends with
        // no rows.
        const bool pendingHasNonLineContent = !pending.errors.empty() || !pending.localLineOffsets.empty();
        if (pendingPrimed || keys.Size() > prevKeyCount || pendingHasNonLineContent)
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
    }

    sink.OnFinished(stopToken.stop_requested());
}

} // namespace loglib::detail
