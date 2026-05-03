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

/// Coalescing thresholds for the live-tail streaming loop. Smaller than the
/// static pipeline's `kStaticBatch*` because the live-tail target is
/// end-to-end latency rather than million-line throughput; the loop emits a
/// batch as soon as either threshold is reached.
constexpr size_t kStreamingBatchFlushLines = 250;
constexpr auto kStreamingBatchFlushInterval = std::chrono::milliseconds(100);

/// Read buffer size for the live-tail loop. 64 KiB matches the pre-fill
/// chunk size in `TailingBytesProducer` and is small enough that each
/// `BytesProducer::Read` returns within a couple of poll ticks even on
/// slow CI runners.
constexpr size_t kStreamingReadBufferSize = 64 * 1024;

/// Format-agnostic live-tail entry point that emits the unified `LogLine`
/// row type backed by a long-lived `StreamLineSource`. Drains
/// `source.Producer()` line-by-line, hands each non-blank line to
/// @p decoder (which must satisfy the `CompactLineDecoder` concept --
/// see `internal/line_decoder.hpp`), atomically commits the resulting
/// `(rawText, ownedArena)` to @p source, and constructs a `LogLine`
/// referencing the just-published `lineId`. Coalesces rows into
/// `StreamedBatch::lines` and honours the `LogParseSink` contract
/// (up to `kStreamingBatchFlushLines` rows or
/// `kStreamingBatchFlushInterval` per batch).
///
/// Single-threaded by design -- the live-tail target is thousands of
/// lines/s, not millions, so the TBB pipeline overhead is not warranted.
///
/// `source` is mutated on the parser thread (`AppendLine` /
/// `AppendOwnedBytes`) and read concurrently from the GUI thread via
/// `LogLine::Source()->RawLine` / `ResolveOwnedBytes`. The
/// `StreamLineSource`'s deque storage and per-source mutex make this
/// safe.
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
        // No byte producer attached -- typical for unit tests that drive
        // the source's API directly. Honour the sink contract with one
        // empty terminal batch and finish.
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

    // Reused per line: cleared at the top of every `processLine`. Move-
    // transferred into the source on success so the source's deque
    // storage owns the bytes thereafter.
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

        // Compact values produced by the decoder are unsorted (the JSON
        // path emits them in source order); the `LogLine` hot-path
        // ctor's `is_sorted` debug assertion needs ascending KeyIds.
        std::sort(compactValues.begin(), compactValues.end(), [](const auto &a, const auto &b) {
            return a.first < b.first;
        });

        // Atomic commit to the source: `AppendLine` move-transfers both
        // the raw bytes and the per-line owned arena under the source's
        // mutex, so any concurrent reader on the GUI thread observes
        // the line either fully written or not at all.
        const size_t lineId = source.AppendLine(std::string(trimmed), std::move(ownedArena));
        ownedArena.clear();

        LogLine logLine(std::move(compactValues), keys, source, lineId);
        // Inline timestamp promotion mirrors the static pipeline. The
        // `string_view{}` arena tells `ExtractStringBytes` to resolve
        // `OwnedString` payloads through the line's `LineSource *` --
        // which, for stream sources, dispatches to
        // `StreamLineSource::ResolveOwnedBytes`.
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
