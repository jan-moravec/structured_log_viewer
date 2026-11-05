/**
 * @brief Line loop for parsing a live stream.
 *
 * The loop reads bytes from the producer, cuts them into lines, runs the format's decoder, folds continuation lines
 * into records and flushes batches to the sink on a timer. It is the live-stream counterpart of the static pipeline.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/bytes_producer.hpp"
#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/stop_token.hpp"
#include "loglib/core/stream_line_source.hpp"
#include "loglib/parse/internal/batch_coalescer.hpp"
#include "loglib/parse/internal/line_decoder.hpp"
#include "loglib/parse/internal/parse_runtime.hpp"
#include "loglib/parse/log_parse_sink.hpp"
#include "loglib/parse/parser_options.hpp"
#include "loglib/time/internal/timestamp_promotion.hpp"

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

/**
 * @brief When the streaming loop sends a batch on: after this many lines, or after this much time. They are smaller
 * than for the static pipeline, because a stream is tuned for delay and not for throughput.
 */
constexpr size_t STREAMING_BATCH_FLUSH_LINES = 250;
constexpr auto STREAMING_BATCH_FLUSH_INTERVAL = std::chrono::milliseconds(100);

/**
 * @brief How long a multi-line record may stay hidden while the loop waits for the next header or the end of the
 * stream.
 */
constexpr auto STREAMING_PENDING_RECORD_MAX_AGE = std::chrono::milliseconds(500);

/**
 * @brief The size of one read from the producer. It equals the read-back chunk of TailingBytesProducer and is small
 * enough for a read to return promptly.
 */
constexpr size_t STREAMING_READ_BUFFER_SIZE = 64 * 1024;

/**
 * @brief Adds continuation text, with a separator in front, to a field of a record.
 *
 * A value that lives in the mapped file needs mmapBytes and is first copied into ownedArena. A field that has no text
 * yet, such as a bare key=, gets the text without a separator.
 */
inline ContinuationSpliceOutcome ExtendContinuationTarget(
    std::vector<std::pair<KeyId, CompactLogValue>> &values,
    std::string &ownedArena,
    KeyId targetKey,
    std::string_view continuationBytes,
    std::string_view mmapBytes = std::string_view{}
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
            // A bare key= has no preceding content to separate.
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

/**
 * @brief Parses a live stream line by line for any format.
 *
 * The loop reads lines from source.Producer() and hands each non-blank line to the decoder, which must satisfy
 * CompactLineDecoder. It stores the raw text and the decoded strings in the source and puts the rows into batches,
 * which are sent on after STREAMING_BATCH_FLUSH_LINES lines or STREAMING_BATCH_FLUSH_INTERVAL.
 *
 * It runs on one thread, because a stream delivers thousands of lines per second and a thread pool would not pay off.
 * The source is changed on the parser thread and read by other threads at the same time, and the mutex and deque
 * storage of StreamLineSource make that safe.
 *
 * A Continue result adds the line to the raw text of the pending record and to the field that
 * decoder.LastContinuationTarget names. A pending record is committed on Emit, at the end of the stream, or when it has
 * waited STREAMING_PENDING_RECORD_MAX_AGE.
 *
 * The loop announces the keys that decoders look up through the worker key cache, before each flush, and the
 * declaredKeys before the first row. A parser that interns its keys before calling, as the regex parser does for the
 * groups of its pattern, passes them as declaredKeys, so those columns appear even if no row has a value.
 */
template <class Decoder>
void RunStreamingParseLoop(
    StreamLineSource &source,
    Decoder &decoder,
    LogParseSink &sink,
    const ParserOptions &options,
    std::span<const KeyId> declaredKeys = {}
)
{
    sink.OnStarted();

    KeyIndex &keys = sink.Keys();
    BatchCoalescer coalescer(sink, keys, STREAMING_BATCH_FLUSH_LINES, STREAMING_BATCH_FLUSH_INTERVAL);
    coalescer.AddSeenKeys(declaredKeys);

    const std::vector<TimeColumnSpec> timeColumns = BuildTimeColumnSpecs(
        keys,
        options.timeColumns,
        options.defaultSourceTimeZone,
        options.referenceTime.value_or(std::chrono::system_clock::now())
    );
    std::span<const TimeColumnSpec> timeColumnsSpan(timeColumns);
    coalescer.SetConvertedTimeKeys(ConvertedTimeKeys(timeColumnsSpan));

    BytesProducer *producer = source.Producer();
    if (producer == nullptr)
    {
        // No producer (e.g. unit tests driving AppendLine directly).
        // Honour the sink contract with one empty terminal batch.
        coalescer.Finish(0, false);
        return;
    }

    const StopToken stopToken = options.stopToken;

    WorkerScratchBase promoteScratch;
    promoteScratch.EnsureTimeColumnCapacity(timeColumns.size());

    size_t nextLineNumber = 1;

    // Bytes that an earlier stage, such as format detection, read from the producer and gave back to the source. They
    // come before anything the producer delivers now.
    std::string carry = source.TakeUnreadBytes();
    std::vector<char> readBuffer(STREAMING_READ_BUFFER_SIZE);
    // Bytes of carry already searched for a newline.
    size_t carrySearched = 0;
    // Set while discarding the rest of a line past maxLineBytes.
    bool skippingOversizedLine = false;
    const size_t maxLineBytes = options.maxLineBytes;

    // Reused per line; move-transferred into the source on success.
    std::vector<std::pair<KeyId, CompactLogValue>> compactValues;
    std::string ownedArena;
    std::string lineError;

    /**
     * @brief A record kept outside the coalescer until AppendLine gives it its final line id. It can still change
     * while continuation lines arrive.
     */
    struct PendingRecord
    {
        std::string rawText;
        std::string ownedArena;
        std::vector<std::pair<KeyId, CompactLogValue>> compactValues;
        size_t startLineNumber = 0;
        KeyId continuationTarget = INVALID_KEY_ID;
        // The continuation text for the target field, each part with a separator in front. Blank lines are left out.
        std::string continuationBytes;
        // The raw text that follows the record. Unlike the field value it keeps blank lines between continuations.
        std::string rawTextAppendix;
        // Blanks become record content only if another continuation follows.
        size_t pendingBlanks = 0;
        std::chrono::steady_clock::time_point openedAt;
        /**
         * @brief True when continuation text can be buffered. For an invalid target the lines are counted and dropped,
         * so memory stays bounded.
         */
        bool continuationAcceptsText = false;
        /**
         * @brief The number of continuation lines dropped because of an invalid target, reported in one summary when
         * the record is committed.
         */
        size_t droppedContinuationLines = 0;
    };
    std::optional<PendingRecord> pending;

    // Announces the keys the decoder met since the last call. This runs before every flush, so a batch always lists the
    // keys of its rows. A key is recorded once for the whole stream, because the loop never starts a new key batch.
    auto announceSeenKeys = [&]() {
        coalescer.AddSeenKeys(promoteScratch.keyCache.firstSeen);
        promoteScratch.keyCache.firstSeen.clear();
    };

    // Reject invalid targets before buffering to keep memory bounded.
    auto canAcceptContinuation = [](std::span<const std::pair<KeyId, CompactLogValue>> values, KeyId key) -> bool {
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
        if (orphanRunFirstLine == orphanRunLastLine)
        {
            coalescer.Pending().errors.push_back(
                ParseError{
                    .firstLine = orphanRunFirstLine,
                    .lastLine = orphanRunFirstLine,
                    .message = "Orphaned continuation line.",
                }
            );
        }
        else
        {
            coalescer.Pending().errors.push_back(
                ParseError{
                    .firstLine = orphanRunFirstLine,
                    .lastLine = orphanRunLastLine,
                    .message = fmt::format(
                        "{} orphaned continuation lines dropped.", orphanRunLastLine - orphanRunFirstLine + 1
                    ),
                }
            );
        }
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
            const ContinuationSpliceOutcome outcome = ExtendContinuationTarget(
                rec.compactValues, rec.ownedArena, rec.continuationTarget, rec.continuationBytes
            );
            if (outcome == ContinuationSpliceOutcome::Ok)
            {
                rec.rawText.append(rec.rawTextAppendix);
            }
            else
            {
                // Defensive: buffered targets should already be valid.
                const std::string_view reason = outcome == ContinuationSpliceOutcome::MissingTarget
                                                    ? "target field is not present in the record"
                                                    : "target field is not a string";
                coalescer.Pending().errors.push_back(
                    ParseError{
                        .firstLine = rec.startLineNumber,
                        .lastLine = rec.startLineNumber,
                        .message = fmt::format("Continuation lines dropped: {}.", reason),
                    }
                );
            }
        }
        // Trailing blanks remain between-record separators.
        if (rec.droppedContinuationLines > 0)
        {
            coalescer.Pending().errors.push_back(
                ParseError{
                    .firstLine = rec.startLineNumber,
                    .lastLine = rec.startLineNumber,
                    .message = fmt::format(
                        "Continuation lines dropped ({} line{}): target field is not present in the record or is not "
                        "a string.",
                        rec.droppedContinuationLines,
                        rec.droppedContinuationLines == 1 ? "" : "s"
                    ),
                }
            );
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
            decoder.DecodeCompact(trimmed, keys, promoteScratch.keyCache, compactValues, ownedArena, lineError);
        if (result == LineDecodeResult::Error)
        {
            flushOrphanRun();
            coalescer.Pending().errors.push_back(
                ParseError{.firstLine = lineNumber, .lastLine = lineNumber, .message = lineError}
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
                if (pending->continuationAcceptsText &&
                    pending->continuationBytes.size() + trimmed.size() >= maxLineBytes)
                {
                    pending->continuationAcceptsText = false;
                }
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
        // Keep new result variants from silently falling through as Emit.
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
        // Decoders that return Continue must expose its target.
        if constexpr (requires { decoder.LastContinuationTarget(); })
        {
            fresh.continuationTarget = decoder.LastContinuationTarget();
        }
        fresh.continuationAcceptsText = canAcceptContinuation(
            std::span<const std::pair<KeyId, CompactLogValue>>(fresh.compactValues), fresh.continuationTarget
        );
        pending = std::move(fresh);

        // Reset moved-from buffers explicitly for clang-analyzer.
        compactValues.clear();
        ownedArena.clear();
    };

    // Emit complete lines already present in the unread bytes before waiting for more producer bytes.
    auto scanCarry = [&]() {
        if (skippingOversizedLine)
        {
            const size_t newline = carry.find('\n');
            if (newline == std::string::npos)
            {
                carry.clear();
                carrySearched = 0;
                return;
            }
            carry.erase(0, newline + 1);
            carrySearched = 0;
            skippingOversizedLine = false;
        }
        size_t scanStart = 0;
        while (scanStart < carry.size())
        {
            const size_t newlineRel = carry.find('\n', std::max(scanStart, carrySearched));
            if (newlineRel == std::string::npos)
            {
                break;
            }
            std::string_view line(carry.data() + scanStart, newlineRel - scanStart);
            processLine(line);
            scanStart = newlineRel + 1;
            carrySearched = 0;

            if (stopToken.StopRequested())
            {
                break;
            }
        }
        if (scanStart > 0)
        {
            carry.erase(0, scanStart);
        }
        carrySearched = carry.size();
        if (carry.size() > maxLineBytes)
        {
            flushOrphanRun();
            coalescer.Pending().errors.push_back(
                ParseError{
                    .firstLine = nextLineNumber,
                    .lastLine = nextLineNumber,
                    .message = fmt::format("Line exceeds the {}-byte limit; dropped.", maxLineBytes),
                }
            );
            ++nextLineNumber;
            carry.clear();
            carrySearched = 0;
            skippingOversizedLine = true;
        }
    };

    if (!carry.empty() && !stopToken.StopRequested())
    {
        scanCarry();
        announceSeenKeys();
        coalescer.TryFlush(false, nextLineNumber - 1);
    }

    bool reachedEof = false;
    while (!reachedEof)
    {
        if (stopToken.StopRequested())
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
                announceSeenKeys();
                coalescer.TryFlush(false, nextLineNumber - 1);
                producer->WaitForBytes(STREAMING_BATCH_FLUSH_INTERVAL);
                continue;
            }
        }

        scanCarry();

        announceSeenKeys();
        coalescer.TryFlush(false, nextLineNumber - 1);
    }

    if (!carry.empty() && !stopToken.StopRequested())
    {
        std::string_view line(carry);
        processLine(line);
        carry.clear();
    }

    // Flush pending records and orphan summaries before the terminal batch.
    flushOrphanRun();
    commitPending();

    announceSeenKeys();
    coalescer.Finish(nextLineNumber - 1, stopToken.StopRequested());
}

} // namespace loglib::internal
