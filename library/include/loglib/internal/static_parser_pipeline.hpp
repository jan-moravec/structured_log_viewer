#pragma once

#include "loglib/file_line_source.hpp"
#include "loglib/internal/advanced_parser_options.hpp"
#include "loglib/internal/batch_coalescer.hpp"
#include "loglib/internal/compact_log_value.hpp"
#include "loglib/internal/line_decoder.hpp"
#include "loglib/internal/parse_runtime.hpp"
#include "loglib/internal/timestamp_promotion.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_parse_sink.hpp"
#include "loglib/parser_options.hpp"
#include "loglib/stop_token.hpp"

#include <fmt/format.h>

#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/parallel_pipeline.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace loglib::internal
{

/// Coalescing thresholds for the static TBB pipeline.
constexpr size_t STATIC_BATCH_FLUSH_LINES = 1000;
constexpr auto STATIC_BATCH_FLUSH_INTERVAL = std::chrono::milliseconds(50);

/// Stage B per-line error. `relativeLine` is 1-based within the batch;
/// Stage C composes the absolute "Error on line N: ..." wrapper using
/// its running line-number cursor.
struct ParsedLineError
{
    size_t relativeLine = 0;
    std::string body;
};

/// Stage B output. Stage C re-asserts ordering before the sink sees it.
struct ParsedPipelineBatch
{
    uint64_t batchIndex = 0;
    std::vector<LogLine> lines;
    std::vector<uint64_t> localLineOffsets;
    std::vector<ParsedLineError> errors;
    /// Per-batch owned-string staging. Stage B appends escape-decoded
    /// bytes; Stage C concatenates into the `LogFile` arena and
    /// rebases the offsets on `lines` in one pass.
    std::string ownedStringsArena;
    /// Source lines consumed (parsed + errors + skipped empties);
    /// advances Stage C's line-number cursor across batches.
    size_t totalLineCount = 0;
    /// Newline-separated continuation bytes at the start of this
    /// batch, to be spliced into the preceding batch's held record.
    std::string leadingContinuationBytes;
    /// Number of physical lines through the last leading continuation.
    /// Includes preceding/interspersed blanks, but excludes blanks
    /// between the last continuation and the next header.
    size_t leadingContinuationLineCount = 0;
    /// Whether Stage C must hold the final record for possible
    /// continuations from the next batch.
    bool lastRecordOpenForContinuation = false;
    /// String field extended on the held final record.
    KeyId continuationTargetKeyId = INVALID_KEY_ID;
    /// Batch-relative physical index of the held record's last
    /// content line. Valid only when `lastRecordOpenForContinuation`.
    size_t tailRecordLastPhysicalLine = 0;
    /// Batch-relative physical index of the held record's header.
    size_t tailRecordHeaderPhysicalLine = 0;

    /// Batch-relative spans for multi-line records sealed within this
    /// batch. Stage C converts them to absolute indices.
    struct MultiLineSpan
    {
        size_t headerPhysicalLine = 0;
        size_t lastPhysicalLine = 0;
    };
    std::vector<MultiLineSpan> completedMultiLineSpans;
};

/// Resolved defaults for `effectiveThreads` and `ntokens`. Both >= 1.
struct ResolvedPipelineSettings
{
    unsigned int effectiveThreads = 1;
    size_t ntokens = 0;
};

ResolvedPipelineSettings ResolvePipelineSettings(const AdvancedParserOptions &advanced);

/// Append @p leadingContinuationBytes to @p heldLine's `targetKey`.
/// Arena-tail values extend in place to keep records spanning many
/// batches linear; other values are copied and rebased at the tail.
inline ContinuationSpliceOutcome SpliceCrossBatchContinuation(
    LogLine &heldLine, LogFile &file, KeyId targetKey, std::string_view leadingContinuationBytes
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
    if (slot->tag != CompactTag::OwnedString && slot->tag != CompactTag::MmapSlice
        && slot->tag != CompactTag::Monostate)
    {
        return ContinuationSpliceOutcome::NonStringTarget;
    }

    const std::string_view arenaView = file.OwnedStringsView();

    // Avoid repeatedly copying records that span several batches.
    if (slot->tag == CompactTag::OwnedString && slot->payload + slot->aux == arenaView.size())
    {
        std::string extension;
        extension.reserve(1 + leadingContinuationBytes.size());
        extension.push_back('\n');
        extension.append(leadingContinuationBytes);
        (void)file.AppendOwnedStrings(extension);
        slot->aux = static_cast<uint32_t>(static_cast<size_t>(slot->aux) + extension.size());
        return ContinuationSpliceOutcome::Ok;
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
    // A bare `key=` has no preceding content to separate.
    if (slot->tag != CompactTag::Monostate)
    {
        joined.push_back('\n');
    }
    joined.append(leadingContinuationBytes);

    const uint64_t offset = file.AppendOwnedStrings(joined);
    slot->tag = CompactTag::OwnedString;
    slot->payload = offset;
    slot->aux = static_cast<uint32_t>(joined.size());
    return ContinuationSpliceOutcome::Ok;
}

/// Static-file TBB pipeline. Stage A (`serial_in_order`) drives
/// tokens; Stage B (`parallel`) decodes; Stage C (`serial_in_order`)
/// rebases per-batch arenas into `source.File()`'s session-global
/// arena, coalesces, diffs new keys, runs inline timestamp promotion,
/// and honours `stop_token`. Stage B stamps each emitted `LogLine` with
/// `&source` and its absolute `lineId`.
///
/// @p newKeyBaseline forwards to `BatchCoalescer` (see its docstring);
/// parsers that intern their schema before the pipeline pass the
/// pre-intern key count. Defaults to the index's current size.
// `stageADriver` / `stageBDecoder` are forwarding refs to keep both
// lvalue and rvalue callables callable without an explicit `std::move`
// at the call site, but they are captured by the inner `[&]` lambdas
// and invoked many times by TBB's pipeline. `std::forward<...>` would
// be UB on the second invocation when the original argument was an
// rvalue, so we deliberately do not forward and silence the warnings.
template <class Token, class UserState, class StageADriver, class StageBDecoder>
void RunStaticParserPipeline(
    FileLineSource &source,
    LogParseSink &sink,
    const ParserOptions &options,
    const AdvancedParserOptions &advanced,
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
    StageADriver &&stageADriver,
    // NOLINTNEXTLINE(cppcoreguidelines-missing-std-forward)
    StageBDecoder &&stageBDecoder,
    std::optional<size_t> newKeyBaseline = std::nullopt
)
{
    LogFile &file = source.File();

    sink.OnStarted();

    KeyIndex &keys = sink.Keys();
    BatchCoalescer coalescer(sink, keys, STATIC_BATCH_FLUSH_LINES, STATIC_BATCH_FLUSH_INTERVAL, newKeyBaseline);

    // Sink contract: at least one `OnBatch` before `OnFinished` on
    // every early-exit path.
    if (options.stopToken.stop_requested())
    {
        coalescer.Finish(1, true);
        return;
    }

    if (file.Size() == 0 || file.Data() == nullptr)
    {
        coalescer.Finish(1, false);
        return;
    }

    const ResolvedPipelineSettings settings = ResolvePipelineSettings(advanced);
    file.ReserveLineOffsets(file.Size() / 100);

    const std::vector<TimeColumnSpec> timeColumns = BuildTimeColumnSpecs(keys, options.configuration.get());

    oneapi::tbb::enumerable_thread_specific<WorkerScratch<UserState>> workers;

    const bool prefersUncoalesced = sink.PrefersUncoalesced();
    size_t nextLineNumber = 1;

    // Stage C holds a batch's open final record until the next batch
    // determines whether more continuation text follows.
    struct HeldTail
    {
        LogLine line;
        std::vector<uint64_t> lineOffsets; // One offset per physical line in the record.
        KeyId continuationTargetKeyId = INVALID_KEY_ID;
        size_t absoluteLineNumber = 0; // header's absolute line number for `Prime`
        size_t headerLineIdx = 0;      // 0-based absolute physical-line index (mLineOffsets index)
        size_t lastLineIdx = 0;        // 0-based absolute physical-line index of the record's last line
    };
    std::optional<HeldTail> held;

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
        // `AppendLineOffsets`; registering here would race readers and
        // may reference a terminating offset not appended yet.
        std::vector<MultiLineRecordSpan> spansThisBatch;
        if (!parsed.completedMultiLineSpans.empty())
        {
            spansThisBatch.reserve(parsed.completedMultiLineSpans.size());
            for (const auto &span : parsed.completedMultiLineSpans)
            {
                spansThisBatch.push_back(MultiLineRecordSpan{
                    lineNumberDelta + span.headerPhysicalLine,
                    lineNumberDelta + span.lastPhysicalLine,
                });
            }
        }

        // Rebase per-batch `OwnedString` offsets into the `LogFile`
        // arena. Stage C is serial_in_order, so this write is
        // single-threaded.
        if (!parsed.ownedStringsArena.empty())
        {
            const uint64_t delta = file.AppendOwnedStrings(parsed.ownedStringsArena);
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
                    held->line, file, held->continuationTargetKeyId, parsed.leadingContinuationBytes
                );
                if (outcome != ContinuationSpliceOutcome::Ok)
                {
                    // Preserve the record and report the dropped bytes.
                    parsed.errors.push_back(ParsedLineError{
                        .relativeLine = 1,
                        .body = outcome == ContinuationSpliceOutcome::MissingTarget
                            ? "Continuation lines dropped: target field is not present in the record."
                            : "Continuation lines dropped: target field is not a string.",
                    });
                }
                // Preserve one offset per physical line and in source
                // order; `LogLine::LineId` and multi-line spans index
                // the same offset array.
                if (parsed.leadingContinuationLineCount > 0)
                {
                    const size_t drop = std::min(
                        parsed.leadingContinuationLineCount, parsed.localLineOffsets.size()
                    );
                    if (drop > 0)
                    {
                        held->lineOffsets.insert(
                            held->lineOffsets.end(),
                            parsed.localLineOffsets.begin(),
                            parsed.localLineOffsets.begin() + static_cast<std::ptrdiff_t>(drop)
                        );
                        parsed.localLineOffsets.erase(
                            parsed.localLineOffsets.begin(),
                            parsed.localLineOffsets.begin() + static_cast<std::ptrdiff_t>(drop)
                        );
                    }
                    held->lastLineIdx = lineNumberDelta + parsed.leadingContinuationLineCount - 1;
                }
            }
            else
            {
                // Keep orphan offsets so subsequent physical line IDs
                // remain aligned with `mLineOffsets`.
                parsed.errors.push_back(ParsedLineError{
                    .relativeLine = 1,
                    .body = "Orphaned continuation line(s) at start of file.",
                });
            }
        }

        // A fresh record seals the held tail. Emit both together so
        // the held span's terminating offset is available at registration.
        const bool batchHasNewRecords = !parsed.lines.empty();
        std::optional<LogLine> heldLineToEmit;
        std::vector<uint64_t> heldOffsetsToEmit;
        std::optional<MultiLineRecordSpan> heldSpanToEmit;
        size_t heldAbsoluteLineNumber = 0;
        if (batchHasNewRecords && held.has_value())
        {
            heldAbsoluteLineNumber = held->absoluteLineNumber;
            if (held->lastLineIdx > held->headerLineIdx)
            {
                heldSpanToEmit = MultiLineRecordSpan{held->headerLineIdx, held->lastLineIdx};
            }
            heldOffsetsToEmit = std::move(held->lineOffsets);
            heldLineToEmit = std::move(held->line);
            held.reset();
        }

        // Compose absolute "Error on line N: ..." here so error and
        // line numbering stay in lockstep with `ShiftLineId` above.
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

        // Remove an open final record from this batch until it is sealed.
        std::optional<HeldTail> newHeld;
        if (batchHasNewRecords && parsed.lastRecordOpenForContinuation)
        {
            LogLine tailLine = std::move(parsed.lines.back());
            parsed.lines.pop_back();
            std::vector<uint64_t> tailOffsets;
            if (!parsed.localLineOffsets.empty())
            {
                tailOffsets.push_back(parsed.localLineOffsets.back());
                parsed.localLineOffsets.pop_back();
            }
            const size_t absoluteLine = tailLine.LineId() + 1; // Prime is 1-based
            const size_t headerAbs = lineNumberDelta + parsed.tailRecordHeaderPhysicalLine;
            const size_t lastAbs = lineNumberDelta + parsed.tailRecordLastPhysicalLine;
            newHeld = HeldTail{
                std::move(tailLine),
                std::move(tailOffsets),
                parsed.continuationTargetKeyId,
                absoluteLine,
                headerAbs,
                lastAbs,
            };
        }

        if (prefersUncoalesced)
        {
            StreamedBatch out;
            // A held record precedes this batch's fresh records.
            if (heldLineToEmit.has_value())
            {
                out.lines.reserve(1 + parsed.lines.size());
                out.lines.push_back(std::move(*heldLineToEmit));
            }
            std::ranges::move(parsed.lines, std::back_inserter(out.lines));
            if (!heldOffsetsToEmit.empty())
            {
                out.localLineOffsets.reserve(heldOffsetsToEmit.size() + parsed.localLineOffsets.size());
                std::ranges::move(heldOffsetsToEmit, std::back_inserter(out.localLineOffsets));
            }
            std::ranges::move(parsed.localLineOffsets, std::back_inserter(out.localLineOffsets));
            if (heldSpanToEmit.has_value())
            {
                spansThisBatch.insert(spansThisBatch.begin(), *heldSpanToEmit);
            }
            out.multiLineSpans = std::move(spansThisBatch);
            formatErrorsInto(out.errors);
            out.firstLineNumber = heldLineToEmit.has_value() ? heldAbsoluteLineNumber : nextLineNumber;
            coalescer.DrainNewKeysInto(out);

            nextLineNumber += parsed.totalLineCount;

            if (!out.lines.empty() || !out.errors.empty() || !out.localLineOffsets.empty()
                || !out.multiLineSpans.empty())
            {
                sink.OnBatch(std::move(out));
            }
            if (batchHasNewRecords)
            {
                held = std::move(newHeld);
            }
            return;
        }

        StreamedBatch &pending = coalescer.Pending();

        // Prime with the earliest record represented in this output.
        if (heldLineToEmit.has_value())
        {
            coalescer.Prime(heldAbsoluteLineNumber);
            pending.lines.push_back(std::move(*heldLineToEmit));
            if (!heldOffsetsToEmit.empty())
            {
                pending.localLineOffsets.insert(
                    pending.localLineOffsets.end(),
                    std::make_move_iterator(heldOffsetsToEmit.begin()),
                    std::make_move_iterator(heldOffsetsToEmit.end())
                );
            }
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
        formatErrorsInto(pending.errors);

        nextLineNumber += parsed.totalLineCount;

        coalescer.TryFlush(false);
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

    // EOF seals the final held record; all of its offsets are present.
    if (held.has_value())
    {
        const MultiLineRecordSpan span{held->headerLineIdx, held->lastLineIdx};
        const bool isMultiLine = span.lastLineId > span.headerLineId;

        if (prefersUncoalesced)
        {
            StreamedBatch out;
            out.lines.push_back(std::move(held->line));
            out.localLineOffsets = std::move(held->lineOffsets);
            if (isMultiLine)
            {
                out.multiLineSpans.push_back(span);
            }
            out.firstLineNumber = held->absoluteLineNumber;
            coalescer.DrainNewKeysInto(out);
            sink.OnBatch(std::move(out));
        }
        else
        {
            StreamedBatch &pending = coalescer.Pending();
            coalescer.Prime(held->absoluteLineNumber);
            pending.lines.push_back(std::move(held->line));
            if (!held->lineOffsets.empty())
            {
                pending.localLineOffsets.insert(
                    pending.localLineOffsets.end(),
                    std::make_move_iterator(held->lineOffsets.begin()),
                    std::make_move_iterator(held->lineOffsets.end())
                );
            }
            if (isMultiLine)
            {
                pending.multiLineSpans.push_back(span);
            }
        }
        held.reset();
    }

    coalescer.Finish(nextLineNumber, stopToken.stop_requested());
}

} // namespace loglib::internal
