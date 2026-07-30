#pragma once

#include "loglib/file_line_source.hpp"
#include "loglib/internal/advanced_parser_options.hpp"
#include "loglib/internal/batch_coalescer.hpp"
#include "loglib/internal/compact_log_value.hpp"
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

#include <chrono>
#include <cstddef>
#include <cstdint>
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
    /// Source lines consumed by `leadingContinuationBytes`. Kept
    /// separate from `totalLineCount` (which counts them too) purely
    /// for auditing / future accounting; Stage C uses `totalLineCount`
    /// to advance its line-number cursor.
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
    /// Stage C converts this to an absolute line-index for
    /// `LogFile::RegisterMultiLineRecord`.
    size_t tailRecordLastPhysicalLine = 0;
    /// 0-based physical-line index (within this batch) of the tail
    /// record's HEADER line. Same meaningfulness rule as
    /// `tailRecordLastPhysicalLine`.
    size_t tailRecordHeaderPhysicalLine = 0;
};

/// Resolved defaults for `effectiveThreads` and `ntokens`. Both >= 1.
struct ResolvedPipelineSettings
{
    unsigned int effectiveThreads = 1;
    size_t ntokens = 0;
};

ResolvedPipelineSettings ResolvePipelineSettings(const AdvancedParserOptions &advanced);

/// Splice @p leadingContinuationBytes into @p heldLine's `targetKey`
/// field, copying the field's existing bytes + a '\n' separator + the
/// new bytes to the tail of @p file's owned-strings arena. Returns
/// true when the splice succeeded; false leaves @p heldLine unchanged
/// (target key missing, non-string field, or empty continuation).
///
/// The old bytes at the field's previous position remain in the arena
/// but become orphaned. Called once per cross-batch splice, so the
/// amortised overhead per record is one arena copy of the last field.
inline bool SpliceCrossBatchContinuation(
    LogLine &heldLine, LogFile &file, KeyId targetKey, std::string_view leadingContinuationBytes
)
{
    if (targetKey == INVALID_KEY_ID || leadingContinuationBytes.empty())
    {
        return targetKey != INVALID_KEY_ID; // empty continuation is a no-op success
    }
    CompactLogValue *slot = heldLine.FindCompactMutable(targetKey);
    if (slot == nullptr)
    {
        return false;
    }
    // Only string-typed fields can absorb continuation text.
    if (slot->tag != CompactTag::OwnedString && slot->tag != CompactTag::MmapSlice
        && slot->tag != CompactTag::Monostate)
    {
        return false;
    }

    std::string joined;
    // Retrieve current bytes (empty if Monostate).
    std::string_view current{};
    if (slot->tag != CompactTag::Monostate)
    {
        auto view = heldLine.PeekStringView(*slot);
        if (!view.has_value())
        {
            return false;
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
    return true;
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

    // Commit a held record to the sink pathway. Called on:
    //   - The next batch arrival (after any splice), or
    //   - End-of-pipeline (`Finish` fallthrough).
    auto commitHeld = [&]() {
        if (!held.has_value())
        {
            return;
        }
        // Register the record's multi-line span so `LogFile::GetLine`
        // (and hence `FileLineSource::RawLine`) returns the joined
        // bytes for the whole record. Single-line records (last ==
        // header) are a no-op inside `RegisterMultiLineRecord`.
        file.RegisterMultiLineRecord(held->headerLineIdx, held->lastLineIdx);

        if (prefersUncoalesced)
        {
            StreamedBatch out;
            out.lines.push_back(std::move(held->line));
            out.localLineOffsets = std::move(held->lineOffsets);
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
                    pending.localLineOffsets.end(), held->lineOffsets.begin(), held->lineOffsets.end()
                );
            }
            coalescer.TryFlush(false);
        }
        held.reset();
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
                const bool ok = SpliceCrossBatchContinuation(
                    held->line, file, held->continuationTargetKeyId, parsed.leadingContinuationBytes
                );
                if (!ok)
                {
                    // Non-string target or missing field: surface one
                    // error, drop the bytes. The held record still
                    // emits with just its own contents.
                    parsed.errors.push_back(ParsedLineError{
                        .relativeLine = 1,
                        .body = "Continuation lines dropped: target field is not a string.",
                    });
                }
                // Extend the held record's byte span so `RawLine`
                // covers the joined text: point the record's stored
                // trailing offset at the LAST leading-continuation
                // line's post-newline offset. Also grow the tracked
                // `lastLineIdx` so `LogFile::RegisterMultiLineRecord`
                // covers the newly-absorbed physical lines.
                if (parsed.leadingContinuationLineCount > 0)
                {
                    if (!held->lineOffsets.empty() && !parsed.localLineOffsets.empty())
                    {
                        const size_t idx =
                            std::min(parsed.leadingContinuationLineCount, parsed.localLineOffsets.size()) - 1;
                        held->lineOffsets.back() = parsed.localLineOffsets[idx];
                    }
                    // Physical lines 0..leadingContinuationLineCount-1
                    // within this batch correspond to absolute indices
                    // (nextLineNumber - 1) .. (nextLineNumber - 1 +
                    // leadingContinuationLineCount - 1).
                    held->lastLineIdx =
                        (nextLineNumber - 1) + parsed.leadingContinuationLineCount - 1;
                }
            }
            else
            {
                // Orphaned continuation at the very start of the file
                // (no prior record to attach to). Surface as an
                // error; the bytes themselves are dropped.
                parsed.errors.push_back(ParsedLineError{
                    .relativeLine = 1,
                    .body = "Orphaned continuation line(s) at start of file.",
                });
            }
            // The leading continuation lines have already had their
            // line-offset entries pushed by Stage B; drop them from
            // this batch's offsets so they don't double-count.
            if (parsed.leadingContinuationLineCount > 0 && !parsed.localLineOffsets.empty())
            {
                const size_t drop =
                    std::min(parsed.leadingContinuationLineCount, parsed.localLineOffsets.size());
                parsed.localLineOffsets.erase(
                    parsed.localLineOffsets.begin(),
                    parsed.localLineOffsets.begin() + static_cast<std::ptrdiff_t>(drop)
                );
            }
        }

        // Commit the held record only when this batch has produced
        // NEW records — i.e., a fresh header line ended the
        // continuation run. All-continuation batches leave the held
        // record open for the next batch to splice into.
        const bool batchHasNewRecords = !parsed.lines.empty();
        if (batchHasNewRecords && held.has_value())
        {
            commitHeld();
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
            const size_t headerAbs = (nextLineNumber - 1) + parsed.tailRecordHeaderPhysicalLine;
            const size_t lastAbs = (nextLineNumber - 1) + parsed.tailRecordLastPhysicalLine;
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
            out.lines = std::move(parsed.lines);
            out.localLineOffsets = std::move(parsed.localLineOffsets);
            formatErrorsInto(out.errors);
            out.firstLineNumber = nextLineNumber;
            coalescer.DrainNewKeysInto(out);

            nextLineNumber += parsed.totalLineCount;

            if (!out.lines.empty() || !out.errors.empty() || !out.localLineOffsets.empty())
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

        // Defer Prime until a real line lands: `firstLineNumber` must
        // be the absolute id of the first source line in the batch,
        // not of an all-error/all-blank prefix chunk.
        if (!parsed.lines.empty())
        {
            coalescer.Prime(nextLineNumber);
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
    commitHeld();

    coalescer.Finish(nextLineNumber, stopToken.stop_requested());
}

} // namespace loglib::internal
