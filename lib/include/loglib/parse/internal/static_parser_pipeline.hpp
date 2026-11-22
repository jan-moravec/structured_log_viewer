/**
 * @brief Parallel pipeline for parsing a memory-mapped file.
 *
 * The pipeline cuts the file into line-aligned batches, decodes them on worker threads with the line decoder of the
 * format, and then delivers the results in file order on a single thread before they reach the sink. A parser plugs in
 * only a LineDecoder and reuses the rest.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/file_line_source.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/stop_token.hpp"
#include "loglib/parse/internal/batch_coalescer.hpp"
#include "loglib/parse/internal/batch_decode.hpp"
#include "loglib/parse/internal/decoded_batch.hpp"
#include "loglib/parse/internal/line_decoder.hpp"
#include "loglib/parse/internal/parse_runtime.hpp"
#include "loglib/parse/log_parse_sink.hpp"
#include "loglib/parse/parser_options.hpp"
#include "loglib/time/internal/timestamp_promotion.hpp"

#include <fmt/format.h>

#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/parallel_pipeline.h>
#include <oneapi/tbb/task_arena.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace loglib::internal
{

/** @brief When the static pipeline sends a batch on: after this many lines, or after this much time. */
constexpr size_t STATIC_BATCH_FLUSH_LINES = 1000;
constexpr auto STATIC_BATCH_FLUSH_INTERVAL = std::chrono::milliseconds(50);

/** @brief The number of threads and of batches in flight after applying defaults. Both are at least 1. */
struct ResolvedPipelineSettings
{
    unsigned int effectiveThreads = 1;
    size_t ntokens = 0;
};

ResolvedPipelineSettings ResolvePipelineSettings(const ParserOptions &options);

/**
 * @brief Adds continuation text from the next batch to a field of the held record.
 *
 * A value at the end of the arena is extended in place, which keeps the cost linear for records that span many batches.
 * Any other value is copied to the end of the arena and then extended.
 */
ContinuationSpliceOutcome SpliceCrossBatchContinuation(
    LogLine &heldLine, FileLineSource &source, KeyId targetKey, std::string_view leadingContinuationBytes
);

/**
 * @brief The body of RunStaticParserPipeline, after the sink was started. It may throw.
 *
 * nextLineNumber is the line counter. The caller reads it after a throw, to say how far the run got.
 */
template <LineDecoder Decoder, class MakeDecoder>
void RunStaticParserPipelineBody(
    FileLineSource &source,
    LogParseSink &sink,
    const ParserOptions &options,
    const MakeDecoder &makeDecoder,
    BatchCoalescer &coalescer,
    size_t &nextLineNumber
)
{
    KeyIndex &keys = sink.Keys();

    const auto referenceTime = options.referenceTime.value_or(std::chrono::system_clock::now());
    const auto makeSnapshot = [&](std::span<const Column> columns) {
        const auto snapshot = std::make_shared<TimeColumnSnapshot>();
        snapshot->specs = BuildTimeColumnSpecs(keys, columns, options.defaultSourceTimeZone, referenceTime);
        snapshot->convertedKeys = ConvertedTimeKeys(snapshot->specs);
        return snapshot;
    };
    std::vector<Column> knownTimeColumns = options.timeColumns;
    std::mutex snapshotMutex;
    std::shared_ptr<const TimeColumnSnapshot> currentTimeColumns = makeSnapshot(knownTimeColumns);

    coalescer.SetConvertedTimeKeys(currentTimeColumns->convertedKeys);

    // Sink contract: at least one OnBatch before OnFinished on
    // every early-exit path.
    if (options.stopToken.StopRequested())
    {
        coalescer.Finish(0, true);
        return;
    }

    if (source.StableBytes().empty())
    {
        coalescer.Finish(0, false);
        return;
    }

    const ResolvedPipelineSettings settings = ResolvePipelineSettings(options);
    // Guess of the average line length in bytes, used only to size the offsets table up front.
    constexpr size_t ASSUMED_BYTES_PER_LINE = 100;
    source.ReserveLineOffsets(source.StableBytes().size() / ASSUMED_BYTES_PER_LINE);

    // Each worker thread has its own scratch and its own decoder, which owns the scratch of its format.
    struct Worker
    {
        WorkerScratch scratch;
        Decoder decoder;
    };
    oneapi::tbb::enumerable_thread_specific<Worker> workers([&] {
        return Worker{.scratch = {}, .decoder = makeDecoder()};
    });

    // The delivery stage holds a batch's open final record until the next batch
    // determines whether more continuation text follows. Line offsets are
    // never held: they stay in physical-line order however rows are delayed.
    struct HeldTail
    {
        LogLine line;
        KeyId continuationTargetKeyId = INVALID_KEY_ID;
        std::shared_ptr<const TimeColumnSnapshot> timeColumns;
        size_t absoluteLineNumber = 0; // header's absolute line number for Prime
        size_t headerLineIdx = 0;      // 0-based absolute physical-line index (mLineOffsets index)
        size_t lastLineIdx = 0;        // 0-based absolute physical-line index of the record's last line
    };
    std::optional<HeldTail> held;

    // A run of continuation lines with no record to join, which may span several batches. It is reported as one error
    // when something else follows it, as the streaming loop does.
    std::optional<ParseError> orphanRun;

    const StopToken stopToken = options.stopToken;

    // Cuts the file into line-aligned batches, in order. It stops cutting when the stop token fires.
    const std::span<const char> fileBytes = source.StableBytes();
    const size_t batchSize = options.batchSizeBytes != 0 ? options.batchSizeBytes : DEFAULT_PARSE_BATCH_BYTES;
    const char *fileBegin = fileBytes.data();
    auto nextBatch =
        MakeLineAlignedBatcher(fileBegin, fileBegin != nullptr ? fileBegin + fileBytes.size() : nullptr, batchSize);
    auto cutBatches = [&](oneapi::tbb::flow_control &fc) -> ByteRange {
        if (stopToken.StopRequested())
        {
            fc.stop();
            return ByteRange{};
        }
        ByteRange range{};
        if (!nextBatch(range))
        {
            fc.stop();
            return ByteRange{};
        }
        return range;
    };

    // Decodes one batch on a worker thread. Every row gets its source and a line id counted inside the batch, and the
    // time columns are converted.
    auto decodeBatch = [&](ByteRange token) -> DecodedBatch {
        Worker &worker = workers.local();
        std::shared_ptr<const TimeColumnSnapshot> timeColumns;
        {
            const std::scoped_lock lock(snapshotMutex);
            timeColumns = currentTimeColumns;
        }
        worker.scratch.EnsureTimeColumnCapacity(timeColumns->specs.size());

        worker.scratch.keyCache.BeginBatch();
        DecodedBatch parsed;
        DecodeBatch(
            token,
            worker.decoder,
            worker.scratch,
            keys,
            source,
            std::span<const TimeColumnSpec>(timeColumns->specs),
            parsed
        );
        parsed.firstSeenKeys = worker.scratch.keyCache.TakeFirstSeen();
        parsed.timeColumns = std::move(timeColumns);

        return parsed;
    };

    // Asks the sink for Time columns it detected and gives the workers a new snapshot. A column with a key that is
    // converted already is skipped. Batches that are being parsed keep the old snapshot, and the table converts their
    // rows itself.
    auto publishTimeColumns = [&] {
        std::vector<Column> added = sink.TimeColumnsAdded();
        bool changed = false;
        for (Column &column : added)
        {
            if (column.type != ColumnType::Time)
            {
                continue;
            }
            const auto &converted = currentTimeColumns->convertedKeys;
            const bool known = std::ranges::any_of(column.keys, [&](const std::string &key) {
                const KeyId id = keys.Find(key);
                return id != INVALID_KEY_ID && std::ranges::binary_search(converted, id);
            });
            if (!known)
            {
                knownTimeColumns.push_back(std::move(column));
                changed = true;
            }
        }
        if (changed)
        {
            auto snapshot = makeSnapshot(knownTimeColumns);
            const std::scoped_lock lock(snapshotMutex);
            currentTimeColumns = std::move(snapshot);
        }
    };

    // Puts the batches back in file order: shifts line ids to file positions, moves the batch's strings to the arena of
    // the file, joins continuation text across batches, coalesces and calls the sink.
    auto deliverInOrder = [&](DecodedBatch parsed) {
        // The rows of this batch borrow their fields from this arena, so the source takes it before any row moves on.
        source.AdoptFieldArena(std::move(parsed.fields));
        const size_t lineNumberDelta = nextLineNumber - 1;
        if (lineNumberDelta != 0)
        {
            for (LogLine &line : parsed.lines)
            {
                line.ShiftLineId(lineNumberDelta);
            }
        }

        // Registration belongs to the sink thread and must follow
        // AppendLineOffsets; registering here would race readers and
        // may reference a terminating offset not appended yet.
        std::vector<MultiLineRecordSpan> spansThisBatch;
        if (!parsed.completedMultiLineSpans.empty())
        {
            spansThisBatch.reserve(parsed.completedMultiLineSpans.size());
            for (const auto &span : parsed.completedMultiLineSpans)
            {
                spansThisBatch.push_back(
                    MultiLineRecordSpan{
                        .headerLineId = lineNumberDelta + span.headerPhysicalLine,
                        .lastLineId = lineNumberDelta + span.lastPhysicalLine,
                    }
                );
            }
        }

        // Rebase per-batch OwnedString offsets into the FileLineSource
        // arena. The delivery stage runs in order on one thread, so this write is
        // single-threaded.
        if (!parsed.ownedStringsArena.empty())
        {
            const uint64_t delta = source.AppendOwnedBytes(parsed.ownedStringsArena);
            for (LogLine &line : parsed.lines)
            {
                line.RebaseOwnedStringOffsets(delta);
            }
        }

        // Splice leading continuations before deciding whether the
        // held record can be emitted.
        if (!parsed.leadingContinuationBytes.empty())
        {
            if (held.has_value())
            {
                const ContinuationSpliceOutcome outcome = SpliceCrossBatchContinuation(
                    held->line, source, held->continuationTargetKeyId, parsed.leadingContinuationBytes
                );
                if (outcome != ContinuationSpliceOutcome::Ok)
                {
                    // Preserve the record and report the dropped bytes.
                    parsed.errors.push_back(
                        ParsedLineError{
                            .relativeLine = 1,
                            .body = outcome == ContinuationSpliceOutcome::MissingTarget
                                        ? "Continuation lines dropped: target field is not present in the record."
                                        : "Continuation lines dropped: target field is not a string.",
                        }
                    );
                }
                if (parsed.leadingContinuationLineCount > 0)
                {
                    held->lastLineIdx = lineNumberDelta + parsed.leadingContinuationLineCount - 1;
                }
            }
            else if (parsed.leadingContinuationLineCount > 0)
            {
                const size_t lastLine = lineNumberDelta + parsed.leadingContinuationLineCount;
                if (orphanRun.has_value())
                {
                    orphanRun->lastLine = lastLine;
                }
                else
                {
                    orphanRun = MakeOrphanedContinuationError(lineNumberDelta + 1, lastLine);
                }
            }
        }

        // A fresh record seals the held tail.
        const bool batchHasNewRecords = !parsed.lines.empty();
        std::optional<LogLine> heldLineToEmit;
        std::optional<MultiLineRecordSpan> heldSpanToEmit;
        std::shared_ptr<const TimeColumnSnapshot> heldTimeColumns;
        size_t heldAbsoluteLineNumber = 0;
        if (batchHasNewRecords && held.has_value())
        {
            heldAbsoluteLineNumber = held->absoluteLineNumber;
            if (held->lastLineIdx > held->headerLineIdx)
            {
                heldSpanToEmit =
                    MultiLineRecordSpan{.headerLineId = held->headerLineIdx, .lastLineId = held->lastLineIdx};
            }
            heldLineToEmit = std::move(held->line);
            heldTimeColumns = std::move(held->timeColumns);
            held.reset();
        }

        // Build the absolute ParseErrors here so error and line numbering stay in lockstep with ShiftLineId above.
        auto moveErrorsInto = [&](std::vector<ParseError> &out) {
            if (parsed.errors.empty())
            {
                return;
            }
            out.reserve(out.size() + parsed.errors.size());
            for (auto &err : parsed.errors)
            {
                const std::uint64_t line = err.relativeLine + lineNumberDelta;
                out.push_back(ParseError{.firstLine = line, .lastLine = line, .message = std::move(err.body)});
            }
            parsed.errors.clear();
        };

        // Remove an open final record from this batch until it is sealed.
        std::optional<HeldTail> newHeld;
        if (batchHasNewRecords && parsed.lastRecordOpenForContinuation)
        {
            LogLine tailLine = std::move(parsed.lines.back());
            parsed.lines.pop_back();
            const size_t absoluteLine = tailLine.LineId() + 1; // Prime is 1-based
            const size_t headerAbs = lineNumberDelta + parsed.tailRecordHeaderPhysicalLine;
            const size_t lastAbs = lineNumberDelta + parsed.tailRecordLastPhysicalLine;
            newHeld = HeldTail{
                std::move(tailLine),
                parsed.continuationTargetKeyId,
                parsed.timeColumns,
                absoluteLine,
                headerAbs,
                lastAbs,
            };
        }

        RowBatch &pending = coalescer.Pending();
        coalescer.AddSeenKeys(parsed.firstSeenKeys);

        // Prime with the earliest record represented in this output.
        if (heldLineToEmit.has_value())
        {
            coalescer.Prime(heldAbsoluteLineNumber);
            coalescer.RestrictConvertedTimeKeys(heldTimeColumns->convertedKeys);
            pending.lines.push_back(std::move(*heldLineToEmit));
            if (heldSpanToEmit.has_value())
            {
                pending.multiLineSpans.push_back(*heldSpanToEmit);
            }
        }

        // Prime from the first emitted row, skipping leading errors,
        // blanks, and continuations that emitted no row.
        if (!parsed.lines.empty())
        {
            const size_t firstAbsolute = nextLineNumber + parsed.leadingContinuationLineCount;
            coalescer.Prime(firstAbsolute);
            coalescer.RestrictConvertedTimeKeys(parsed.timeColumns->convertedKeys);
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
        if (!spansThisBatch.empty())
        {
            pending.multiLineSpans.insert(
                pending.multiLineSpans.end(),
                std::make_move_iterator(spansThisBatch.begin()),
                std::make_move_iterator(spansThisBatch.end())
            );
        }
        if (orphanRun.has_value() && (batchHasNewRecords || !parsed.errors.empty()))
        {
            pending.errors.push_back(MakeOrphanedContinuationError(orphanRun->firstLine, orphanRun->lastLine));
            orphanRun.reset();
        }
        moveErrorsInto(pending.errors);

        nextLineNumber += parsed.totalLineCount;

        const bool flushed = coalescer.TryFlush(false, nextLineNumber - 1);
        if (batchHasNewRecords)
        {
            held = std::move(newHeld);
        }
        if (flushed)
        {
            publishTimeColumns();
        }
    };

    // The pipeline runs in its own arena, so its thread limit applies to this parse only. Other TBB users in the
    // process, such as a filter running for another tab, keep their own concurrency.
    oneapi::tbb::task_arena arena(static_cast<int>(settings.effectiveThreads));

    // Moves the open final record and the open orphan run into the batch being built.
    auto sealHeldTail = [&] {
        if (orphanRun.has_value())
        {
            coalescer.Pending().errors.push_back(
                MakeOrphanedContinuationError(orphanRun->firstLine, orphanRun->lastLine)
            );
            orphanRun.reset();
        }
        if (held.has_value())
        {
            const MultiLineRecordSpan span{held->headerLineIdx, held->lastLineIdx};
            const bool isMultiLine = span.lastLineId > span.headerLineId;

            RowBatch &pending = coalescer.Pending();
            coalescer.Prime(held->absoluteLineNumber);
            coalescer.RestrictConvertedTimeKeys(held->timeColumns->convertedKeys);
            pending.lines.push_back(std::move(held->line));
            if (isMultiLine)
            {
                pending.multiLineSpans.push_back(span);
            }
            held.reset();
        }
    };

    try
    {
        arena.execute([&] {
            oneapi::tbb::parallel_pipeline(
                settings.ntokens,
                oneapi::tbb::make_filter<void, ByteRange>(oneapi::tbb::filter_mode::serial_in_order, cutBatches) &
                    oneapi::tbb::make_filter<ByteRange, DecodedBatch>(oneapi::tbb::filter_mode::parallel, decodeBatch) &
                    oneapi::tbb::make_filter<DecodedBatch, void>(
                        oneapi::tbb::filter_mode::serial_in_order, deliverInOrder
                    )
            );
        });
    }
    catch (...)
    {
        // The held record holds the lines read so far and is a valid row. Send it before the failure is reported.
        sealHeldTail();
        throw;
    }

    // EOF seals the final held record.
    sealHeldTail();

    coalescer.Finish(nextLineNumber - 1, stopToken.StopRequested());
}

/**
 * @brief Parses a memory-mapped file in parallel and sends the result to the sink in file order.
 *
 * - The first stage runs in order and cuts the file into batches. It stops cutting when the stop token fires.
 * - The second stage runs in parallel and decodes the batches with the line decoder that makeDecoder returns for each
 *   worker. It gives every row its source and a line id counted inside the batch, and converts the time columns.
 * - The third stage runs in order. It shifts the line ids to file positions, appends each batch's strings to the arena
 *   of the file, coalesces batches, announces the new keys and calls the sink.
 *
 * The decoding stage records the keys it meets first in each batch, and the third stage announces them in batch order,
 * so the new keys come in the order they first appear in the file, however the workers were scheduled. declaredKeys are
 * announced before the first row. A parser that interns its keys before the pipeline starts, such as CSV with its
 * header, passes them there so they appear even if no row has a value.
 *
 * When the parse throws, the sink still gets one last batch with an error that says so and the record that was still
 * open, and OnFinished, and the exception goes on to the caller. See BatchCoalescer::FinishAfterException. TBB cancels
 * the pipeline and rethrows a stage's exception on the calling thread, so the sink is never called from two threads at
 * once.
 */
template <LineDecoder Decoder, class MakeDecoder>
void RunStaticParserPipeline(
    FileLineSource &source,
    LogParseSink &sink,
    const ParserOptions &options,
    const MakeDecoder &makeDecoder,
    std::span<const KeyId> declaredKeys = {}
)
{
    sink.OnStarted();

    BatchCoalescer coalescer(sink, sink.Keys(), STATIC_BATCH_FLUSH_LINES, STATIC_BATCH_FLUSH_INTERVAL);
    coalescer.AddSeenKeys(declaredKeys);
    size_t nextLineNumber = 1;

    try
    {
        RunStaticParserPipelineBody<Decoder>(source, sink, options, makeDecoder, coalescer, nextLineNumber);
    }
    catch (...)
    {
        coalescer.FinishAfterException(nextLineNumber - 1, std::current_exception());
        throw;
    }
}

} // namespace loglib::internal
