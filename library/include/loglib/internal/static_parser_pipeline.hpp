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
    /// Bytes for continuation lines at the START of this batch. They
    /// logically belong to the previous batch's last record and must be
    /// spliced there by Stage C. Includes internal '\n' separators
    /// (i.e., "line1\nline2\nline3"). Empty when the batch does not
    /// open on a continuation. Only ever populated by parsers running
    /// in a multi-line mode.
    std::string leadingContinuationBytes;
    /// Physical source lines consumed by the leading-continuation
    /// region: from the first line of the batch through the LAST
    /// leading continuation (inclusive). Interspersed blank lines
    /// count toward this total when they sit BETWEEN or BEFORE
    /// leading continuations, matching the in-batch semantics where
    /// a blank line between the header and a continuation is folded
    /// into the multi-line span. Blank lines AFTER the last leading
    /// continuation (i.e. immediately before the batch's first fresh
    /// header) are NOT counted -- they belong to no record, exactly
    /// like a blank between two single-line headers. Stage C uses
    /// this to (a) transfer that many leading offsets from
    /// `localLineOffsets` onto the held record's `lineOffsets` and
    /// (b) compute `held->lastLineIdx = lineNumberDelta + count - 1`.
    size_t leadingContinuationLineCount = 0;
    /// True when this batch's LAST record may still receive
    /// continuations from the NEXT batch (multi-line mode active AND
    /// the batch ends on a record). Stage C holds that record back
    /// until it sees the next batch's `leadingContinuationBytes`.
    bool lastRecordOpenForContinuation = false;
    /// KeyId to receive continuation-byte extensions for the LAST
    /// record of this batch. `INVALID_KEY_ID` when
    /// `lastRecordOpenForContinuation` is false. The receiving field
    /// must be string-typed (validated by the parser at intern time).
    KeyId continuationTargetKeyId = INVALID_KEY_ID;
    /// 0-based physical-line index (within this batch) of the tail
    /// record's LAST content line — header for a single-line record,
    /// or the last folded-in continuation for a multi-line record.
    /// Meaningful only when `lastRecordOpenForContinuation == true`;
    /// Stage C converts this to an absolute line-index carried on the
    /// `StreamedBatch::multiLineSpans` it emits (the sink registers
    /// spans after `AppendLineOffsets`, so the widening `GetLine` can
    /// find its terminating offset).
    size_t tailRecordLastPhysicalLine = 0;
    /// 0-based physical-line index (within this batch) of the tail
    /// record's HEADER line. Same meaningfulness rule as
    /// `tailRecordLastPhysicalLine`.
    size_t tailRecordHeaderPhysicalLine = 0;

    /// Completed in-batch multi-line records (records that received
    /// at least one continuation and were then sealed by a fresh
    /// header inside the same batch). Stage B pushes {header, last}
    /// per record; Stage C converts each to absolute line indices
    /// and attaches them to the emitted `StreamedBatch::multiLineSpans`
    /// vector. Without this side-channel, only the batch's TAIL
    /// record ever gets its span registered -- non-tail multi-line
    /// records would render through `RawLine` as their header line
    /// only, dropping the continuation bytes from Copy Line, Record
    /// Details, and any consumer that goes through
    /// `LineSource::RawLine`.
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

/// Splice @p leadingContinuationBytes into @p heldLine's `targetKey`
/// field. When the field already lives at the tail of @p file's
/// owned-strings arena we extend it in place; otherwise we copy the
/// field's existing bytes + a '\n' separator + the new bytes to the
/// tail. Outcome is shared with the streaming path's
/// `ExtendContinuationTarget` so both surface the same error text.
///
/// Which path fires depends on the batch that PRECEDES this call:
/// - Fast path (in-place append). Only reachable when the incoming
///   batch's `ownedStringsArena` is empty, so Stage C skipped the
///   `AppendOwnedStrings` that would have pushed the held field off
///   the arena tail. In practice: batches consisting SOLELY of
///   leading continuations (`parsed.lines.empty()`), which is the
///   common shape once a stack trace is running longer than the
///   configured batch size.
/// - Slow / middle path (copy the field to the arena tail). Fires
///   every time the incoming batch also had fresh records: Stage C
///   appends the batch arena FIRST, which relocates the held field
///   away from the tail. `SpliceCrossBatchContinuation` then has to
///   materialise the field + new bytes at the new tail.
///
/// Preserving the fast path matters because a stack trace can span
/// N batches -- always relocating O(record) bytes per batch would be
/// O(N^2).
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
    // Only string-typed fields can absorb continuation text.
    if (slot->tag != CompactTag::OwnedString && slot->tag != CompactTag::MmapSlice
        && slot->tag != CompactTag::Monostate)
    {
        return ContinuationSpliceOutcome::NonStringTarget;
    }

    const std::string_view arenaView = file.OwnedStringsView();

    // Fast path: field is already an `OwnedString` at the file arena
    // tail. Append '\n' + new bytes in place without relocating the
    // existing field. Common in the "batch N stashed its tail record,
    // batch N+1 arrives with just leading continuations" pattern.
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
    // Retrieve current bytes (empty if Monostate).
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
    // Skip the leading '\n' separator when the target was Monostate
    // (bare `key=` with no value): the continuation text should be
    // the field's first content, not prefixed by a stray newline.
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

    // Hold-back state for multi-line records. When a batch's last
    // record `lastRecordOpenForContinuation`, Stage C moves it here
    // instead of feeding it to the coalescer / sink so it can absorb
    // any `leadingContinuationBytes` the next batch surfaces.
    struct HeldTail
    {
        LogLine line;
        std::vector<uint64_t> lineOffsets; // localLineOffsets for the physical lines making up this record
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

        // Absolute-index spans accumulated for this batch. Attached
        // to the emitted `StreamedBatch` so `LogFile::RegisterMultiLine`
        // Record` runs on the sink thread *after* the batch's
        // `AppendLineOffsets`. Doing it here on the parser thread
        // would race the UI reader (`GetLine` reads `mMultiLineSpans`
        // without a lock) and, when the terminating offset comes from
        // the *following* batch's non-leading lines, would look up an
        // offset that hasn't been appended yet -- the widened
        // `GetLine` would silently fall back to header-only.
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

        // Cross-batch stitching: if this batch opens on a continuation
        // and we're holding the previous batch's last record, splice
        // the leading bytes into the held record's continuation
        // target BEFORE deciding whether to commit the held record.
        if (!parsed.leadingContinuationBytes.empty())
        {
            if (held.has_value())
            {
                const ContinuationSpliceOutcome outcome = SpliceCrossBatchContinuation(
                    held->line, file, held->continuationTargetKeyId, parsed.leadingContinuationBytes
                );
                if (outcome != ContinuationSpliceOutcome::Ok)
                {
                    // Surface one error, drop the bytes. The held
                    // record still emits with just its own contents.
                    parsed.errors.push_back(ParsedLineError{
                        .relativeLine = 1,
                        .body = outcome == ContinuationSpliceOutcome::MissingTarget
                            ? "Continuation lines dropped: target field is not present in the record."
                            : "Continuation lines dropped: target field is not a string.",
                    });
                }
                // Move the leading-continuation-line offsets from
                // `parsed` onto `held` so every physical line the
                // record now spans lands in `mLineOffsets` in order
                // on the eventual `commitHeld`. Overwriting held's
                // single trailing entry with only the LAST leading
                // offset (and dropping the intermediates) collapses
                // multi-line records into fewer physical-line
                // entries in `mLineOffsets`, which breaks
                // `LogFile::GetLine(continuationLineId)` for the
                // dropped lines and drives `RegisterMultiLineRecord`
                // past the end of the shrunken offsets array (making
                // the widening guard skip the header). See the
                // regression case in `test_regex_parser.cpp` /
                // `test_logfmt_parser.cpp` under
                // `[file_line_source][multiline][cross_batch]`.
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
                    // Physical lines 0..leadingContinuationLineCount-1
                    // within this batch correspond to absolute indices
                    // (nextLineNumber - 1) .. (nextLineNumber - 1 +
                    // leadingContinuationLineCount - 1).
                    held->lastLineIdx = lineNumberDelta + parsed.leadingContinuationLineCount - 1;
                }
            }
            else
            {
                // Orphaned continuation at the very start of the file
                // (no prior record to attach to). Surface as an
                // error and drop the content bytes; keep the
                // per-physical-line offset entries so the following
                // record's `LogLine::LineId` (a physical index) still
                // resolves to the right slot in `mLineOffsets`.
                parsed.errors.push_back(ParsedLineError{
                    .relativeLine = 1,
                    .body = "Orphaned continuation line(s) at start of file.",
                });
            }
        }

        // Fold a held record into this batch's output only when the
        // batch has NEW records -- a fresh header ended the
        // continuation run. All-continuation batches leave the held
        // record open for the next batch to splice into. Emitting
        // the held record in the SAME sink batch as the next batch's
        // fresh records is what keeps its widened stop offset (in
        // this batch's non-leading `localLineOffsets`) available to
        // `LogFile::GetLine(headerLineId)` when the sink registers
        // the span.
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

        // Hold back this batch's tail record if it's still open for
        // continuation. We remove it from `parsed.lines` (and its
        // matching line-offset) so it doesn't reach the sink yet.
        std::optional<HeldTail> newHeld;
        if (batchHasNewRecords && parsed.lastRecordOpenForContinuation)
        {
            LogLine tailLine = std::move(parsed.lines.back());
            parsed.lines.pop_back();
            // Pop the tail record's offset entry (its last physical
            // line's post-newline offset — Stage B pushes one offset
            // per physical line, and in-batch continuations sit
            // between the header and the batch tail).
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
            // Emit held first, then this batch's fresh records; keeps
            // physical-line order intact.
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
            // If we folded a held record in, the batch actually begins
            // at that record; otherwise use this batch's cursor.
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

        // Fold held first so `Prime` fires at its header line
        // number; subsequent Prime calls with the batch's own first
        // line are no-ops (batch coalescer keeps the earliest).
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

        // Defer Prime until a real line lands: `firstLineNumber` must
        // be the absolute id of the first source line in the batch,
        // not of an all-error/all-blank prefix chunk. When this batch
        // opens on leading continuations (which produced no `LogLine`)
        // the first fresh header sits `leadingContinuationLineCount`
        // physical lines in.
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

    // Flush any held tail record so it's not lost when the pipeline
    // reaches EOF without another batch arriving to trigger commit.
    // No further offsets can arrive, so registering the span here is
    // safe: the terminating offset for the record is already in the
    // held offsets we're about to emit.
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
