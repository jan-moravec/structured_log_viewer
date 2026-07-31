#pragma once

#include "loglib/bytes_producer.hpp"
#include "loglib/internal/batch_coalescer.hpp"
#include "loglib/internal/compact_log_value.hpp"
#include "loglib/internal/line_decoder.hpp"
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
#include <cassert>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib::internal
{

/// Coalescing thresholds for the live-tail loop. Tighter than the
/// static pipeline because we optimise for latency, not throughput.
constexpr size_t STREAMING_BATCH_FLUSH_LINES = 250;
constexpr auto STREAMING_BATCH_FLUSH_INTERVAL = std::chrono::milliseconds(100);

/// Maximum time a multi-line record may remain invisible while
/// waiting for another header or EOF.
constexpr auto STREAMING_PENDING_RECORD_MAX_AGE = std::chrono::milliseconds(500);

/// Read buffer size for the live-tail loop. Matches `TailingBytesProducer`'s
/// pre-fill chunk; small enough that each `Read` returns promptly.
constexpr size_t STREAMING_READ_BUFFER_SIZE = 64 * 1024;

/// Extend @p targetKey with separator-prefixed continuation bytes.
/// Mmap-backed values require @p mmapBytes and are materialised into
/// @p ownedArena. A bare `key=` is promoted without a leading separator.
inline ContinuationSpliceOutcome ExtendContinuationTarget(
    std::vector<std::pair<KeyId, CompactLogValue>> &values, std::string &ownedArena, KeyId targetKey,
    std::string_view continuationBytes, std::string_view mmapBytes = std::string_view{}
)
{
    if (targetKey == INVALID_KEY_ID)
    {
        return ContinuationSpliceOutcome::MissingTarget;
    }
    if (continuationBytes.empty())
    {
        return ContinuationSpliceOutcome::Ok;
    }

    for (auto &kv : values)
    {
        if (kv.first != targetKey)
        {
            continue;
        }
        CompactLogValue &v = kv.second;
        switch (v.tag)
        {
            case CompactTag::OwnedString:
            {
                const uint64_t offset = v.payload;
                const uint32_t length = v.aux;
                if (offset + length == ownedArena.size())
                {
                    ownedArena.append(continuationBytes.data(), continuationBytes.size());
                    v.aux = static_cast<uint32_t>(static_cast<size_t>(length) + continuationBytes.size());
                }
                else
                {
                    // Reserve before self-appending: reallocation would
                    // invalidate the source pointer, and overlap safety
                    // is not guaranteed.
                    ownedArena.reserve(ownedArena.size() + length + continuationBytes.size());
                    const uint64_t newOffset = ownedArena.size();
                    ownedArena.append(ownedArena.data() + offset, length);
                    ownedArena.append(continuationBytes.data(), continuationBytes.size());
                    v.payload = newOffset;
                    v.aux = static_cast<uint32_t>(static_cast<size_t>(length) + continuationBytes.size());
                }
                return ContinuationSpliceOutcome::Ok;
            }
            case CompactTag::Monostate:
            {
                // A bare `key=` has no preceding content to separate.
                std::string_view bytes = continuationBytes;
                if (!bytes.empty() && bytes.front() == '\n')
                {
                    bytes.remove_prefix(1);
                }
                v.tag = CompactTag::OwnedString;
                v.payload = ownedArena.size();
                v.aux = static_cast<uint32_t>(bytes.size());
                ownedArena.append(bytes.data(), bytes.size());
                return ContinuationSpliceOutcome::Ok;
            }
            case CompactTag::MmapSlice:
            {
                // Materialise the mmap slice before extending it.
                const uint64_t offset = v.payload;
                const uint32_t length = v.aux;
                if (mmapBytes.empty() || offset + length > mmapBytes.size())
                {
                    return ContinuationSpliceOutcome::NonStringTarget;
                }
                const uint64_t newOffset = ownedArena.size();
                ownedArena.append(mmapBytes.data() + offset, length);
                ownedArena.append(continuationBytes.data(), continuationBytes.size());
                v.tag = CompactTag::OwnedString;
                v.payload = newOffset;
                v.aux = static_cast<uint32_t>(static_cast<size_t>(length) + continuationBytes.size());
                return ContinuationSpliceOutcome::Ok;
            }
            case CompactTag::DictRef:
            case CompactTag::Int64:
            case CompactTag::Uint64:
            case CompactTag::Double:
            case CompactTag::Bool:
            case CompactTag::Timestamp:
                return ContinuationSpliceOutcome::NonStringTarget;
        }
        return ContinuationSpliceOutcome::NonStringTarget;
    }
    return ContinuationSpliceOutcome::MissingTarget;
}

/// Format-agnostic live-tail entry point. Drains `source.Producer()`
/// line-by-line, hands each non-blank line to @p decoder (must
/// satisfy `CompactLineDecoder`), commits `(rawText, ownedArena)` to
/// @p source, and emits `LogLine`s into batches throttled by
/// `STREAMING_BATCH_FLUSH_*`.
///
/// Single-threaded: the target is thousands of lines/s, so TBB
/// overhead is not warranted. `source` is mutated on the parser
/// thread and read concurrently by the GUI; `StreamLineSource`'s
/// internal mutex + deque storage make that safe.
///
/// A `Continue` result extends the pending record's raw text and the
/// field selected by `decoder.LastContinuationTarget()`. Pending
/// records commit on `Emit`, EOF, or the maximum pending age.
/// @p newKeyBaseline forwards to `BatchCoalescer`. Parsers that
/// pre-intern keys into the sink's `KeyIndex` before calling here
/// (e.g. `RegexParser`, whose schema comes from the pattern's
/// named capture groups) pass the snapshot taken *before* the
/// intern step, so eagerly-interned columns still surface as
/// `newKeys` on the first emitted batch. Parsers that add keys
/// lazily (JSON, logfmt, CSV) leave this `std::nullopt`.
template <class Decoder>
void RunStreamingParseLoop(
    StreamLineSource &source,
    Decoder &decoder,
    LogParseSink &sink,
    const ParserOptions &options,
    std::optional<size_t> newKeyBaseline = std::nullopt
)
{
    sink.OnStarted();

    KeyIndex &keys = sink.Keys();
    BatchCoalescer coalescer(sink, keys, STREAMING_BATCH_FLUSH_LINES, STREAMING_BATCH_FLUSH_INTERVAL, newKeyBaseline);

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
    std::vector<char> readBuffer(STREAMING_READ_BUFFER_SIZE);

    // Reused per line; move-transferred into the source on success.
    std::vector<std::pair<KeyId, CompactLogValue>> compactValues;
    std::string ownedArena;
    std::string lineError;

    /// Mutable record held outside the coalescer until `AppendLine`
    /// assigns its stable line ID.
    struct PendingRecord
    {
        std::string rawText;
        std::string ownedArena;
        std::vector<std::pair<KeyId, CompactLogValue>> compactValues;
        size_t startLineNumber = 0;
        KeyId continuationTarget = INVALID_KEY_ID;
        // Separator-prefixed continuation content for the target field.
        // Blank physical lines are intentionally excluded.
        std::string continuationBytes;
        // Raw record suffix; unlike the field value, this preserves
        // blank physical lines between continuations.
        std::string rawTextAppendix;
        // Blanks become record content only if another continuation follows.
        size_t pendingBlanks = 0;
        std::chrono::steady_clock::time_point openedAt;
        /// Whether continuation bytes can be buffered safely. Invalid
        /// targets are counted and dropped to bound memory use.
        bool continuationAcceptsText = false;
        /// Invalid-target continuations summarized at commit.
        size_t droppedContinuationLines = 0;
    };
    std::optional<PendingRecord> pending;

    // Reject invalid targets before buffering to keep memory bounded.
    auto canAcceptContinuation =
        [](std::span<const std::pair<KeyId, CompactLogValue>> values, KeyId key) -> bool {
        if (key == INVALID_KEY_ID)
        {
            return false;
        }
        for (const auto &kv : values)
        {
            if (kv.first != key)
            {
                continue;
            }
            switch (kv.second.tag)
            {
                case CompactTag::OwnedString:
                case CompactTag::Monostate:
                case CompactTag::MmapSlice:
                    return true;
                case CompactTag::DictRef:
                case CompactTag::Int64:
                case CompactTag::Uint64:
                case CompactTag::Double:
                case CompactTag::Bool:
                case CompactTag::Timestamp:
                    return false;
            }
            return false;
        }
        return false;
    };

    // Summarize each consecutive orphan run as one error.
    size_t orphanRunFirstLine = 0;
    size_t orphanRunLastLine = 0;
    auto flushOrphanRun = [&]() {
        if (orphanRunFirstLine == 0)
        {
            return;
        }
        std::string msg;
        if (orphanRunFirstLine == orphanRunLastLine)
        {
            msg = fmt::format("Error on line {}: Orphaned continuation line.", orphanRunFirstLine);
        }
        else
        {
            msg = fmt::format(
                "Error on lines {}-{}: {} orphaned continuation lines dropped.",
                orphanRunFirstLine,
                orphanRunLastLine,
                orphanRunLastLine - orphanRunFirstLine + 1
            );
        }
        coalescer.Pending().errors.emplace_back(std::move(msg));
        orphanRunFirstLine = 0;
        orphanRunLastLine = 0;
    };

    auto commitPending = [&]() {
        if (!pending)
        {
            return;
        }
        PendingRecord rec = std::move(*pending);
        pending.reset();

        if (!rec.continuationBytes.empty())
        {
            const ContinuationSpliceOutcome outcome =
                ExtendContinuationTarget(rec.compactValues, rec.ownedArena, rec.continuationTarget, rec.continuationBytes);
            if (outcome == ContinuationSpliceOutcome::Ok)
            {
                rec.rawText.append(rec.rawTextAppendix);
            }
            else
            {
                // Defensive: buffered targets should already be valid.
                coalescer.Pending().errors.emplace_back(fmt::format(
                    "Continuation lines dropped on line {}: {}.",
                    rec.startLineNumber,
                    outcome == ContinuationSpliceOutcome::MissingTarget
                        ? "target field is not present in the record"
                        : "target field is not a string"
                ));
            }
        }
        // Trailing blanks remain between-record separators.
        if (rec.droppedContinuationLines > 0)
        {
            coalescer.Pending().errors.emplace_back(fmt::format(
                "Continuation lines dropped on line {} ({} line{}): target field is not present in the record or is not a string.",
                rec.startLineNumber,
                rec.droppedContinuationLines,
                rec.droppedContinuationLines == 1 ? "" : "s"
            ));
        }

        std::sort(rec.compactValues.begin(), rec.compactValues.end(), [](const auto &a, const auto &b) {
            return a.first < b.first;
        });

        const size_t lineId = source.AppendLine(std::move(rec.rawText), std::move(rec.ownedArena));
        LogLine logLine(std::move(rec.compactValues), keys, source, lineId);
        // Promote only after all continuations have joined the record.
        promoteScratch.PromoteTimestamps(logLine, timeColumnsSpan, std::string_view{});
        coalescer.Prime(rec.startLineNumber);
        coalescer.Pending().lines.push_back(std::move(logLine));
    };

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
            flushOrphanRun();
            // Delay classifying blanks until another continuation arrives.
            if (pending && pending->continuationAcceptsText)
            {
                ++pending->pendingBlanks;
            }
            return;
        }

        const LineDecodeResult result =
            decoder.DecodeCompact(trimmed, keys, &promoteScratch.keyCache, compactValues, ownedArena, lineError);
        if (result == LineDecodeResult::Error)
        {
            flushOrphanRun();
            coalescer.Pending().errors.emplace_back(
                fmt::format("Error on line {}: {}", lineNumber, std::move(lineError))
            );
            return;
        }
        if (result == LineDecodeResult::Skip)
        {
            flushOrphanRun();
            return;
        }
        if (result == LineDecodeResult::Continue)
        {
            if (pending)
            {
                flushOrphanRun();
                if (pending->continuationAcceptsText)
                {
                    // Preserve blanks in raw text, but not in the target field.
                    if (pending->pendingBlanks > 0)
                    {
                        pending->rawTextAppendix.append(pending->pendingBlanks, '\n');
                        pending->pendingBlanks = 0;
                    }
                    pending->continuationBytes.push_back('\n');
                    pending->continuationBytes.append(trimmed.data(), trimmed.size());
                    pending->rawTextAppendix.push_back('\n');
                    pending->rawTextAppendix.append(trimmed.data(), trimmed.size());
                }
                else
                {
                    // Count invalid-target lines without retaining their bytes.
                    ++pending->droppedContinuationLines;
                    pending->pendingBlanks = 0;
                }
            }
            else
            {
                if (orphanRunFirstLine == 0)
                {
                    orphanRunFirstLine = lineNumber;
                }
                orphanRunLastLine = lineNumber;
            }
            return;
        }
        // Keep new result variants from silently falling through as `Emit`.
        assert(result == LineDecodeResult::Emit);
        if (result != LineDecodeResult::Emit)
        {
            std::unreachable();
        }

        // A new header seals the previous record.
        flushOrphanRun();
        commitPending();

        PendingRecord fresh;
        fresh.rawText.assign(trimmed.data(), trimmed.size());
        fresh.ownedArena = std::move(ownedArena);
        fresh.compactValues = std::move(compactValues);
        fresh.startLineNumber = lineNumber;
        fresh.openedAt = std::chrono::steady_clock::now();
        // Decoders that return `Continue` must expose its target.
        if constexpr (requires { decoder.LastContinuationTarget(); })
        {
            fresh.continuationTarget = decoder.LastContinuationTarget();
        }
        fresh.continuationAcceptsText =
            canAcceptContinuation(std::span<const std::pair<KeyId, CompactLogValue>>(fresh.compactValues), fresh.continuationTarget);
        pending = std::move(fresh);

        // Reset moved-from buffers explicitly for clang-analyzer.
        compactValues.clear();
        ownedArena.clear();
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
                // Bound how long a quiet producer keeps a record invisible.
                if (pending)
                {
                    const auto age = std::chrono::steady_clock::now() - pending->openedAt;
                    if (age >= STREAMING_PENDING_RECORD_MAX_AGE)
                    {
                        commitPending();
                    }
                }
                coalescer.TryFlush(false);
                producer->WaitForBytes(STREAMING_BATCH_FLUSH_INTERVAL);
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

    // Flush pending records and orphan summaries before the terminal batch.
    flushOrphanRun();
    commitPending();

    coalescer.Finish(nextLineNumber, stopToken.stop_requested());
}

} // namespace loglib::internal
