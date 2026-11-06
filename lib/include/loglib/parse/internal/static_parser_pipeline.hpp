/**
 * @brief Parallel pipeline for parsing a memory-mapped file.
 *
 * The pipeline splits the file into batches, decodes them on worker threads, and then reassembles the results in file
 * order on a single thread before they reach the sink. Every static parser plugs in a batch splitter and a decoder and
 * reuses the rest.
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
#include "loglib/parse/internal/line_decoder.hpp"
#include "loglib/parse/internal/parse_runtime.hpp"
#include "loglib/parse/log_parse_sink.hpp"
#include "loglib/parse/parser_options.hpp"
#include "loglib/time/internal/timestamp_promotion.hpp"

#include <fmt/format.h>

#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/parallel_pipeline.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
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

/**
 * @brief An error from decoding one line. relativeLine counts from 1 inside the batch, and Stage C turns it into a
 * ParseError with the absolute line number.
 */
struct ParsedLineError
{
    size_t relativeLine = 0;
    std::string body;
};

/**
 * @brief What Stage B produces for one batch. Stage C puts the batches back in file order before the sink sees them.
 */
struct ParsedPipelineBatch
{
    uint64_t batchIndex = 0;
    std::vector<LogLine> lines;
    std::vector<uint64_t> localLineOffsets;
    std::vector<ParsedLineError> errors;
    /** @brief The keys this batch met first, in the order the decoder met them. Stage C announces them in batch order.
     */
    std::vector<KeyId> firstSeenKeys;
    /**
     * @brief Staging area for the decoded strings of the batch. Stage C appends it to the file's arena and shifts the
     * offsets of the lines in one pass.
     */
    std::string ownedStringsArena;
    /**
     * @brief The number of source lines used by the batch: parsed lines, errors and skipped blank lines. It moves
     * Stage C's line counter on.
     */
    size_t totalLineCount = 0;
    /**
     * @brief Continuation text at the start of the batch, separated by newlines. Stage C adds it to the record held
     * from the previous batch.
     */
    std::string leadingContinuationBytes;
    /**
     * @brief The number of physical lines up to the last leading continuation, counting blank lines before or between
     * them but not blank lines between the last continuation and the next header.
     */
    size_t leadingContinuationLineCount = 0;
    /** @brief True when Stage C must hold the last record, because the next batch may continue it. */
    bool lastRecordOpenForContinuation = false;
    /** @brief The string field of the held record that continuation text is added to. */
    KeyId continuationTargetKeyId = INVALID_KEY_ID;
    /**
     * @brief The line, counted inside the batch, where the held record's content ends. Only valid when
     * lastRecordOpenForContinuation is set.
     */
    size_t tailRecordLastPhysicalLine = 0;
    /** @brief The line, counted inside the batch, where the held record starts. */
    size_t tailRecordHeaderPhysicalLine = 0;

    /**
     * @brief The line ranges of multi-line records that are complete inside this batch, counted inside the batch.
     * Stage C turns them into absolute line numbers.
     */
    struct MultiLineSpan
    {
        size_t headerPhysicalLine = 0;
        size_t lastPhysicalLine = 0;
    };
    std::vector<MultiLineSpan> completedMultiLineSpans;
};

/** @brief The number of threads and of batches in flight after applying defaults. Both are at least 1. */
struct ResolvedPipelineSettings
{
    unsigned int effectiveThreads = 1;
    size_t ntokens = 0;
};

/** @brief One line-aligned slice of the mapped file handed from Stage A to Stage B. */
struct ByteRange
{
    uint64_t batchIndex = 0;
    const char *bytesBegin = nullptr;
    const char *bytesEnd = nullptr;
    const char *fileEnd = nullptr;
};

/**
 * @brief Returns the Stage A source for a mapped file.
 *
 * Each call emits the next slice of about batchSize bytes, extended to the end of its last line, until fileEnd. A null
 * range yields no slices.
 */
inline auto MakeLineAlignedBatcher(const char *fileBegin, const char *fileEnd, size_t batchSize)
{
    return [cursor = fileBegin, fileEnd, batchSize, batchIndex = uint64_t{0}](ByteRange &out) mutable -> bool {
        if (cursor == nullptr || cursor >= fileEnd)
        {
            return false;
        }
        const char *batchBegin = cursor;
        const auto remaining = static_cast<size_t>(fileEnd - cursor);
        // Bounded advance: cursor + batchSize past one-past-end is undefined.
        const size_t advance = std::min(batchSize, remaining);
        const char *target = cursor + advance;
        if (advance < remaining)
        {
            const char *newline =
                static_cast<const char *>(memchr(target, '\n', static_cast<size_t>(fileEnd - target)));
            cursor = (newline != nullptr) ? newline + 1 : fileEnd;
        }
        else
        {
            cursor = fileEnd;
        }

        out.batchIndex = batchIndex++;
        out.bytesBegin = batchBegin;
        out.bytesEnd = cursor;
        out.fileEnd = fileEnd;
        return true;
    };
}

ResolvedPipelineSettings ResolvePipelineSettings(const ParserOptions &options);

/**
 * @brief Adds continuation text from the next batch to a field of the held record.
 *
 * A value at the end of the arena is extended in place, which keeps the cost linear for records that span many batches.
 * Any other value is copied to the end of the arena and then extended.
 */
inline ContinuationSpliceOutcome SpliceCrossBatchContinuation(
    LogLine &heldLine, FileLineSource &source, KeyId targetKey, std::string_view leadingContinuationBytes
)
{
    if (targetKey == INVALID_KEY_ID)
    {
        return ContinuationSpliceOutcome::MissingTarget;
    }
    if (leadingContinuationBytes.empty())
    {
        return ContinuationSpliceOutcome::Ok;
    }
    CompactLogValue *slot = heldLine.FindCompactMutable(targetKey);
    if (slot == nullptr)
    {
        return ContinuationSpliceOutcome::MissingTarget;
    }
    if (slot->tag != CompactTag::OwnedString && slot->tag != CompactTag::MmapSlice &&
        slot->tag != CompactTag::Monostate)
    {
        return ContinuationSpliceOutcome::NonStringTarget;
    }

    // Avoid repeatedly copying records that span several batches.
    if (slot->tag == CompactTag::OwnedString)
    {
        std::string extension;
        extension.reserve(1 + leadingContinuationBytes.size());
        extension.push_back('\n');
        extension.append(leadingContinuationBytes);
        if (source.TryExtendOwnedString(slot->payload, slot->aux, extension))
        {
            slot->aux = static_cast<uint32_t>(static_cast<size_t>(slot->aux) + extension.size());
            return ContinuationSpliceOutcome::Ok;
        }
    }

    std::string joined;
    std::string_view current{};
    if (slot->tag != CompactTag::Monostate)
    {
        auto view = heldLine.PeekStringView(*slot);
        if (!view.has_value())
        {
            return ContinuationSpliceOutcome::NonStringTarget;
        }
        current = *view;
    }
    joined.reserve(current.size() + 1 + leadingContinuationBytes.size());
    joined.append(current);
    // A bare key= has no preceding content to separate.
    if (slot->tag != CompactTag::Monostate)
    {
        joined.push_back('\n');
    }
    joined.append(leadingContinuationBytes);

    const uint64_t offset = source.AppendOwnedBytes(heldLine.LineId(), joined);
    slot->tag = CompactTag::OwnedString;
    slot->payload = offset;
    slot->aux = static_cast<uint32_t>(joined.size());
    return ContinuationSpliceOutcome::Ok;
}

/**
 * @brief The body of RunStaticParserPipeline, after the sink was started. It may throw.
 *
 * nextLineNumber is the line counter. The caller reads it after a throw, to say how far the run got.
 */
// The two callables are taken as forwarding references so that lvalues and rvalues both work without a move at the call
// site. They are used many times inside the pipeline, so they are deliberately not forwarded, because forwarding an
// rvalue more than once would be undefined behavior. The related warnings are suppressed below.
template <class Token, class UserState, class StageADriver, class StageBDecoder>
void RunStaticParserPipelineBody(
    FileLineSource &source,
    LogParseSink &sink,
    const ParserOptions &options,
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
    StageADriver &&stageADriver,
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
    StageBDecoder &&stageBDecoder,
    BatchCoalescer &coalescer,
    size_t &nextLineNumber
)
{
    KeyIndex &keys = sink.Keys();

    const std::vector<TimeColumnSpec> timeColumns = BuildTimeColumnSpecs(
        keys,
        options.timeColumns,
        options.defaultSourceTimeZone,
        options.referenceTime.value_or(std::chrono::system_clock::now())
    );

    coalescer.SetConvertedTimeKeys(ConvertedTimeKeys(timeColumns));

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
    source.ReserveLineOffsets(source.StableBytes().size() / 100);

    oneapi::tbb::enumerable_thread_specific<WorkerScratch<UserState>> workers;

    // Stage C holds a batch's open final record until the next batch
    // determines whether more continuation text follows. Line offsets are
    // never held: they stay in physical-line order however rows are delayed.
    struct HeldTail
    {
        LogLine line;
        KeyId continuationTargetKeyId = INVALID_KEY_ID;
        size_t absoluteLineNumber = 0; // header's absolute line number for Prime
        size_t headerLineIdx = 0;      // 0-based absolute physical-line index (mLineOffsets index)
        size_t lastLineIdx = 0;        // 0-based absolute physical-line index of the record's last line
    };
    std::optional<HeldTail> held;

    const StopToken stopToken = options.stopToken;
    std::span<const TimeColumnSpec> timeColumnsSpan(timeColumns);

    auto stageA = [&](oneapi::tbb::flow_control &fc) -> Token {
        if (stopToken.StopRequested())
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

        worker.keyCache.BeginBatch();
        ParsedPipelineBatch parsed;
        stageBDecoder(std::move(token), worker, keys, timeColumnsSpan, parsed);
        parsed.firstSeenKeys = worker.keyCache.TakeFirstSeen();

        return parsed;
    };

    auto stageC = [&](ParsedPipelineBatch parsed) {
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
        // arena. Stage C is serial_in_order, so this write is
        // single-threaded.
        if (!parsed.ownedStringsArena.empty())
        {
            const uint64_t delta = source.AppendOwnedBytes(nextLineNumber, parsed.ownedStringsArena);
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
            else
            {
                parsed.errors.push_back(
                    ParsedLineError{
                        .relativeLine = 1,
                        .body = "Orphaned continuation line(s) at start of file.",
                    }
                );
            }
        }

        // A fresh record seals the held tail.
        const bool batchHasNewRecords = !parsed.lines.empty();
        std::optional<LogLine> heldLineToEmit;
        std::optional<MultiLineRecordSpan> heldSpanToEmit;
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
        moveErrorsInto(pending.errors);

        nextLineNumber += parsed.totalLineCount;

        coalescer.TryFlush(false, nextLineNumber - 1);
        if (batchHasNewRecords)
        {
            held = std::move(newHeld);
        }
    };

    oneapi::tbb::global_control gc(
        oneapi::tbb::global_control::max_allowed_parallelism, static_cast<size_t>(settings.effectiveThreads)
    );

    oneapi::tbb::parallel_pipeline(
        settings.ntokens,
        oneapi::tbb::make_filter<void, Token>(oneapi::tbb::filter_mode::serial_in_order, stageA) &
            oneapi::tbb::make_filter<Token, ParsedPipelineBatch>(oneapi::tbb::filter_mode::parallel, stageB) &
            oneapi::tbb::make_filter<ParsedPipelineBatch, void>(oneapi::tbb::filter_mode::serial_in_order, stageC)
    );

    // EOF seals the final held record.
    if (held.has_value())
    {
        const MultiLineRecordSpan span{held->headerLineIdx, held->lastLineIdx};
        const bool isMultiLine = span.lastLineId > span.headerLineId;

        RowBatch &pending = coalescer.Pending();
        coalescer.Prime(held->absoluteLineNumber);
        pending.lines.push_back(std::move(held->line));
        if (isMultiLine)
        {
            pending.multiLineSpans.push_back(span);
        }
        held.reset();
    }

    coalescer.Finish(nextLineNumber - 1, stopToken.StopRequested());
}

/**
 * @brief Parses a memory-mapped file in parallel and sends the result to the sink in file order.
 *
 * - Stage A runs in order and cuts the file into batches. It stops cutting when the stop token fires.
 * - Stage B runs in parallel and decodes the batches. It gives every row its source and a line id counted inside the
 *   batch, and converts the time columns.
 * - Stage C runs in order. It shifts the line ids to file positions, appends each batch's strings to the arena of the
 *   file, coalesces batches, announces the new keys and calls the sink.
 *
 * Stage B records the keys it meets first in each batch, and Stage C announces them in batch order, so the new keys
 * come in the order they first appear in the file, however the workers were scheduled. declaredKeys are announced
 * before the first row. A parser that interns its keys before the pipeline starts, such as CSV with its header, passes
 * them there so they appear even if no row has a value.
 *
 * When the parse throws, the sink still gets one last batch with an error that says so, and OnFinished, and the
 * exception goes on to the caller. See BatchCoalescer::FinishAfterException. TBB cancels the pipeline and rethrows a
 * stage's exception on the calling thread, so the sink is never called from two threads at once.
 */
// The callables are forwarded as lvalues to the body, which uses them many times.
template <class Token, class UserState, class StageADriver, class StageBDecoder>
void RunStaticParserPipeline(
    FileLineSource &source,
    LogParseSink &sink,
    const ParserOptions &options,
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
    StageADriver &&stageADriver,
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
    StageBDecoder &&stageBDecoder,
    std::span<const KeyId> declaredKeys = {}
)
{
    sink.OnStarted();

    BatchCoalescer coalescer(sink, sink.Keys(), STATIC_BATCH_FLUSH_LINES, STATIC_BATCH_FLUSH_INTERVAL);
    coalescer.AddSeenKeys(declaredKeys);
    size_t nextLineNumber = 1;

    try
    {
        RunStaticParserPipelineBody<Token, UserState>(
            source, sink, options, stageADriver, stageBDecoder, coalescer, nextLineNumber
        );
    }
    catch (...)
    {
        coalescer.FinishAfterException(nextLineNumber - 1, std::current_exception());
        throw;
    }
}

} // namespace loglib::internal
