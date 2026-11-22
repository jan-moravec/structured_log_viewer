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
#include "loglib/parse/internal/record_assembler.hpp"
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
 * @brief Turns the string values that appear in the stored raw line into slices of it.
 *
 * A stream row keeps its raw line, so a value that is a substring of that line need not be stored a second time. Each
 * such OwnedString field becomes a MmapSlice field that the stream source resolves against the raw line. Values that
 * are not in the line, such as decoded escapes or text added from continuation lines, stay owned, and ownedArena is
 * rebuilt to hold only those.
 */
inline void SliceValuesOfRawLine(
    std::vector<CompactLogValue> &values, std::string &ownedArena, std::string_view rawLine
)
{
    if (ownedArena.empty())
    {
        return;
    }
    const std::string_view arena(ownedArena);
    std::string kept;
    for (CompactLogValue &field : values)
    {
        if (field.tag != CompactTag::OwnedString)
        {
            continue;
        }
        const std::string_view text = arena.substr(field.payload, field.aux);
        const size_t position = text.empty() ? std::string_view::npos : rawLine.find(text);
        if (position != std::string_view::npos)
        {
            field = CompactLogValue::MakeMmapSlice(position, field.aux).WithKey(field.Key());
            continue;
        }
        const uint64_t offset = kept.size();
        kept.append(text);
        field.payload = offset;
    }
    ownedArena = std::move(kept);
}

/**
 * @brief The body of RunStreamingParseLoop, after the sink was started. It may throw.
 *
 * nextLineNumber is the line counter. The caller reads it after a throw, to say how far the run got.
 */
template <class Decoder>
void RunStreamingParseLoopBody(
    StreamLineSource &source,
    Decoder &decoder,
    LogParseSink &sink,
    const ParserOptions &options,
    BatchCoalescer &coalescer,
    size_t &nextLineNumber
)
{
    KeyIndex &keys = sink.Keys();

    // Without a fixed reference time, every batch reads the clock again, so a stream that runs across New Year gives
    // the rows after it the new year. The first row of a batch refreshes the reference time of the time columns.
    const auto readClock = [&options] { return options.clock ? options.clock() : std::chrono::system_clock::now(); };
    std::vector<TimeColumnSpec> timeColumns = BuildTimeColumnSpecs(
        keys, options.timeColumns, options.defaultSourceTimeZone, options.referenceTime.value_or(readClock())
    );
    const std::span<const TimeColumnSpec> timeColumnsSpan(timeColumns);
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

    // Bytes that an earlier stage, such as format detection, read from the producer and gave back to the source. They
    // come before anything the producer delivers now.
    std::string carry = source.TakeUnreadBytes();
    std::vector<char> readBuffer(STREAMING_READ_BUFFER_SIZE);
    // Bytes of carry already searched for a newline.
    size_t carrySearched = 0;
    // Set while discarding the rest of a line past maxLineBytes.
    bool skippingOversizedLine = false;
    const size_t maxLineBytes = options.maxLineBytes;

    // Reused per line; the open record takes them when the line starts a record.
    std::vector<CompactLogValue> compactValues;
    std::string ownedArena;
    std::string lineError;
    LineContext context{keys, promoteScratch.keyCache, ownedArena, std::span<const char>{}, 0};

    // The record that is held back until the next header, the end of the stream or its age limit. The assembler holds
    // its values and continuation text. The raw text and the owned strings are stored in the source when the record is
    // committed, and the record can still change until then.
    RecordAssembler assembler(/*keepRawText=*/true);
    std::string recordRawText;
    std::string recordOwnedBytes;
    std::chrono::steady_clock::time_point recordOpenedAt;
    bool sawFirstLine = false;

    // Announces the keys the decoder met since the last call. This runs before every flush, so a batch always lists the
    // keys of its rows. A key is recorded once for the whole stream, because the loop never starts a new key batch.
    auto announceSeenKeys = [&] {
        coalescer.AddSeenKeys(promoteScratch.keyCache.firstSeen);
        promoteScratch.keyCache.firstSeen.clear();
    };

    // Summarize each consecutive orphan run as one error.
    size_t orphanRunFirstLine = 0;
    size_t orphanRunLastLine = 0;
    auto flushOrphanRun = [&]() {
        if (orphanRunFirstLine == 0)
        {
            return;
        }
        coalescer.Pending().errors.push_back(MakeOrphanedContinuationError(orphanRunFirstLine, orphanRunLastLine));
        orphanRunFirstLine = 0;
        orphanRunLastLine = 0;
    };

    auto commitPending = [&]() {
        if (!assembler.IsOpen())
        {
            return;
        }
        const size_t startLineNumber = assembler.FirstLine();

        if (!assembler.ContinuationText().empty())
        {
            const ContinuationSpliceOutcome outcome = assembler.JoinContinuation(recordOwnedBytes, std::string_view{});
            if (outcome == ContinuationSpliceOutcome::Ok)
            {
                recordRawText.append(assembler.RawContinuationText());
            }
            else
            {
                // Defensive: buffered targets should already be valid.
                const std::string_view reason = outcome == ContinuationSpliceOutcome::MissingTarget
                                                    ? "target field is not present in the record"
                                                    : "target field is not a string";
                coalescer.Pending().errors.push_back(
                    ParseError{
                        .firstLine = startLineNumber,
                        .lastLine = startLineNumber,
                        .message = fmt::format("Continuation lines dropped: {}.", reason),
                    }
                );
            }
        }
        // Trailing blanks remain between-record separators.
        const size_t dropped = assembler.DroppedContinuationLines();
        if (dropped > 0)
        {
            coalescer.Pending().errors.push_back(
                ParseError{
                    .firstLine = startLineNumber,
                    .lastLine = startLineNumber,
                    .message = fmt::format(
                        "Continuation lines dropped ({} line{}): target field is not present in the record or is not "
                        "a string.",
                        dropped,
                        dropped == 1 ? "" : "s"
                    ),
                }
            );
        }

        std::vector<CompactLogValue> recordValues;
        assembler.Close(recordValues);

        SliceValuesOfRawLine(recordValues, recordOwnedBytes, recordRawText);
        const size_t lineId = source.AppendLine(std::move(recordRawText), std::move(recordOwnedBytes));
        recordRawText.clear();
        recordOwnedBytes.clear();
        LogLine logLine(std::move(recordValues), source, lineId);
        if (!options.referenceTime.has_value() && coalescer.Pending().lines.empty())
        {
            const auto now = readClock();
            for (TimeColumnSpec &spec : timeColumns)
            {
                spec.referenceTime = now;
            }
        }
        // Promote only after all continuations have joined the record.
        promoteScratch.PromoteTimestamps(logLine, timeColumnsSpan, std::string_view{});
        coalescer.Prime(startLineNumber);
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

        // The decoder sees the line without a byte order mark, while the stored raw text keeps it.
        std::string_view decodable = trimmed;
        if constexpr (StripsBom<Decoder>())
        {
            if (!sawFirstLine && decodable.starts_with(UTF8_BYTE_ORDER_MARK))
            {
                decodable.remove_prefix(UTF8_BYTE_ORDER_MARK.size());
            }
        }
        sawFirstLine = true;

        if (decodable.empty())
        {
            flushOrphanRun();
            // Delay classifying blanks until another continuation arrives.
            assembler.AddBlank();
            return;
        }

        ownedArena.clear();
        const LineDecodeResult result = decoder.Decode(decodable, context, compactValues, lineError);
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
            if (assembler.IsOpen())
            {
                flushOrphanRun();
                assembler.AddContinuation(decodable, lineNumber, maxLineBytes);
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

        recordRawText.assign(trimmed.data(), trimmed.size());
        recordOwnedBytes = std::move(ownedArena);
        ownedArena.clear();
        recordOpenedAt = std::chrono::steady_clock::now();
        assembler.Open(compactValues, lineNumber, decoder.ContinuationTarget());
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

    // If the producer or the sink fails, the record that is still open holds valid lines. Send it before the failure is
    // reported, so that those lines are not lost.
    try
    {
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
                    if (assembler.IsOpen())
                    {
                        const auto age = std::chrono::steady_clock::now() - recordOpenedAt;
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
    }
    catch (...)
    {
        try
        {
            flushOrphanRun();
            commitPending();
            announceSeenKeys();
        }
        // NOLINTNEXTLINE(bugprone-empty-catch)
        catch (...)
        {
            // The original failure is the one to report.
        }
        throw;
    }

    // Flush pending records and orphan summaries before the terminal batch.
    flushOrphanRun();
    commitPending();

    announceSeenKeys();
    coalescer.Finish(nextLineNumber - 1, stopToken.StopRequested());
}

/**
 * @brief Parses a live stream line by line for any format.
 *
 * The loop reads lines from source.Producer() and hands each non-blank line to the decoder, which must satisfy
 * LineDecoder. It stores the raw text and the decoded strings in the source and puts the rows into batches,
 * which are sent on after STREAMING_BATCH_FLUSH_LINES lines or STREAMING_BATCH_FLUSH_INTERVAL.
 *
 * It runs on one thread, because a stream delivers thousands of lines per second and a thread pool would not pay off.
 * The source is changed on the parser thread and read by other threads at the same time, and the mutex and deque
 * storage of StreamLineSource make that safe.
 *
 * A Continue result adds the line to the raw text of the pending record and to the field that
 * decoder.ContinuationTarget names. A pending record is committed on Emit, at the end of the stream, or when it has
 * waited STREAMING_PENDING_RECORD_MAX_AGE.
 *
 * The loop announces the keys that decoders look up through the worker key cache, before each flush, and the
 * declaredKeys before the first row. A parser that interns its keys before calling, as the regex parser does for the
 * groups of its pattern, passes them as declaredKeys, so those columns appear even if no row has a value.
 *
 * When the loop throws, the sink still gets one last batch with an error that says so and the record that was still
 * open, and OnFinished, and the exception goes on to the caller. See BatchCoalescer::FinishAfterException.
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

    BatchCoalescer coalescer(sink, sink.Keys(), STREAMING_BATCH_FLUSH_LINES, STREAMING_BATCH_FLUSH_INTERVAL);
    coalescer.AddSeenKeys(declaredKeys);
    size_t nextLineNumber = 1;

    try
    {
        RunStreamingParseLoopBody(source, decoder, sink, options, coalescer, nextLineNumber);
    }
    catch (...)
    {
        coalescer.FinishAfterException(nextLineNumber - 1, std::current_exception());
        throw;
    }
}

} // namespace loglib::internal
