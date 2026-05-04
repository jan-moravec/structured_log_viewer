#pragma once

#include "loglib/bytes_producer.hpp"
#include "loglib/internal/batch_coalescer.hpp"
#include "loglib/internal/compact_log_value.hpp"
#include "loglib/internal/parse_runtime.hpp"
#include "loglib/internal/timestamp_promotion.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_parse_sink.hpp"
#include "loglib/parser_options.hpp"
#include "loglib/stop_token.hpp"
#include "loglib/stream_line_source.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib::internal
{

/// Coalescing thresholds for the live-tail loop. Tighter than the
/// static pipeline because we optimise for latency, not throughput.
constexpr size_t kStreamingBatchFlushLines = 250;
constexpr auto kStreamingBatchFlushInterval = std::chrono::milliseconds(100);

/// Read buffer size for the live-tail loop. Matches `TailingBytesProducer`'s
/// pre-fill chunk; small enough that each `Read` returns promptly.
constexpr size_t kStreamingReadBufferSize = 64 * 1024;

/// Format-agnostic live-tail entry point. Drains `source.Producer()`
/// line-by-line, hands each non-blank line to @p decoder (must
/// satisfy `CompactLineDecoder`), commits `(rawText, ownedArena)` to
/// @p source, and emits `LogLine`s into batches throttled by
/// `kStreamingBatchFlush*`.
///
/// Single-threaded: the target is thousands of lines/s, so TBB
/// overhead is not warranted. `source` is mutated on the parser
/// thread and read concurrently by the GUI; `StreamLineSource`'s
/// internal mutex + deque storage make that safe.
template <class Decoder>
void RunStreamingParseLoop(
    StreamLineSource &source, Decoder &decoder, LogParseSink &sink, const ParserOptions &options
)
{
    sink.OnStarted();

    KeyIndex &keys = sink.Keys();
    BatchCoalescer coalescer(sink, keys, kStreamingBatchFlushLines, kStreamingBatchFlushInterval);

    BytesProducer *producer = source.Producer();
    if (producer == nullptr)
    {
        // No producer (e.g. unit tests driving `AppendLine` directly).
        // Honour the sink contract with one empty terminal batch.
        coalescer.Finish(1, false);
        return;
    }

    const StopToken stopToken = options.stopToken;

    const std::vector<TimeColumnSpec> timeColumns = BuildTimeColumnSpecs(keys, options.configuration.get());
    std::span<const TimeColumnSpec> timeColumnsSpan(timeColumns);

    WorkerScratchBase promoteScratch;
    promoteScratch.EnsureTimeColumnCapacity(timeColumns.size());

    size_t nextLineNumber = 1;

    std::string carry;
    std::vector<char> readBuffer(kStreamingReadBufferSize);

    // Reused per line; move-transferred into the source on success.
    std::vector<std::pair<KeyId, CompactLogValue>> compactValues;
    std::string ownedArena;
    std::string lineError;

    auto processLine = [&](std::string_view line) {
        std::string_view trimmed = line;
        if (!trimmed.empty() && trimmed.back() == '\r')
        {
            trimmed.remove_suffix(1);
        }
        const size_t lineNumber = nextLineNumber;
        ++nextLineNumber;

        if (trimmed.empty())
        {
            return;
        }

        const bool ok =
            decoder.DecodeCompact(trimmed, keys, &promoteScratch.keyCache, compactValues, ownedArena, lineError);
        if (!ok)
        {
            coalescer.Pending().errors.emplace_back(
                fmt::format("Error on line {}: {}", lineNumber, std::move(lineError))
            );
            return;
        }

        // `LogLine` ctor's debug `is_sorted` assertion requires
        // ascending KeyIds; decoders may emit in source order.
        std::sort(compactValues.begin(), compactValues.end(), [](const auto &a, const auto &b) {
            return a.first < b.first;
        });

        // Atomic commit: `AppendLine` move-transfers both the raw
        // bytes and the per-line arena under the source's mutex.
        const size_t lineId = source.AppendLine(std::string(trimmed), std::move(ownedArena));
        ownedArena.clear();

        LogLine logLine(std::move(compactValues), keys, source, lineId);
        // Empty arena -> resolution falls through to the line's
        // `LineSource *` (i.e. `StreamLineSource::ResolveOwnedBytes`).
        promoteScratch.PromoteTimestamps(logLine, timeColumnsSpan, std::string_view{});

        coalescer.Prime(lineNumber);
        coalescer.Pending().lines.push_back(std::move(logLine));
    };

    bool reachedEof = false;
    while (!reachedEof)
    {
        if (stopToken.stop_requested())
        {
            break;
        }

        const size_t read = producer->Read(std::span<char>(readBuffer.data(), readBuffer.size()));
        if (read != 0)
        {
            carry.append(readBuffer.data(), read);
        }
        else
        {
            if (producer->IsClosed())
            {
                reachedEof = true;
            }
            else
            {
                coalescer.TryFlush(false);
                producer->WaitForBytes(kStreamingBatchFlushInterval);
                continue;
            }
        }

        size_t scanStart = 0;
        while (scanStart < carry.size())
        {
            const size_t newlineRel = carry.find('\n', scanStart);
            if (newlineRel == std::string::npos)
            {
                break;
            }
            std::string_view line(carry.data() + scanStart, newlineRel - scanStart);
            processLine(line);
            scanStart = newlineRel + 1;

            if (stopToken.stop_requested())
            {
                break;
            }
        }
        if (scanStart > 0)
        {
            carry.erase(0, scanStart);
        }

        coalescer.TryFlush(false);
    }

    if (!carry.empty() && !stopToken.stop_requested())
    {
        std::string_view line(carry);
        processLine(line);
        carry.clear();
    }

    coalescer.Finish(nextLineNumber, stopToken.stop_requested());
}

} // namespace loglib::internal
