#pragma once

#include "loglib/file_line_source.hpp"
#include "loglib/internal/advanced_parser_options.hpp"
#include "loglib/internal/batch_coalescer.hpp"
#include "loglib/internal/parse_runtime.hpp"
#include "loglib/internal/timestamp_promotion.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_parse_sink.hpp"
#include "loglib/parser_options.hpp"
#include "loglib/stop_token.hpp"

#include <fmt/format.h>

#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/parallel_pipeline.h>
#include <oneapi/tbb/enumerable_thread_specific.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace loglib::internal
{

/// Coalescing thresholds for the static TBB pipeline. Picked to keep
/// per-batch GUI-thread work bounded while still soaking up the parser's
/// throughput on bulk static loads.
constexpr size_t kStaticBatchFlushLines = 1000;
constexpr auto kStaticBatchFlushInterval = std::chrono::milliseconds(50);

/// Per-line error emitted by Stage B. `relativeLine` is the 1-based line
/// number within the source batch (i.e. it resets to 1 at every Stage B
/// invocation). Stage C composes the absolute `Error on line N: <body>`
/// string by adding its running line-number cursor -- Stage B cannot do
/// this itself because it has no view of the cumulative line count.
struct ParsedLineError
{
    size_t relativeLine = 0;
    std::string body;
};

/// Parsed Stage B output. Stage C re-asserts ordering before the sink sees it.
struct ParsedPipelineBatch
{
    uint64_t batchIndex = 0;
    std::vector<LogLine> lines;
    std::vector<uint64_t> localLineOffsets;
    std::vector<ParsedLineError> errors;
    /// Per-batch owned-string staging buffer. Stage B appends escape-decoded
    /// bytes here and stamps `OwnedString` payloads with offsets into this
    /// buffer; Stage C concatenates this into the `LogFile`'s arena and
    /// rebases the offsets in `lines` once.
    std::string ownedStringsArena;
    /// Source lines consumed (parsed + errors + skipped empties). Stage C uses
    /// this to advance its running line-number cursor across batches.
    size_t totalLineCount = 0;
};

/// Resolved defaults for `effectiveThreads` and `ntokens`. Both >= 1.
struct ResolvedPipelineSettings
{
    unsigned int effectiveThreads = 1;
    size_t ntokens = 0;
};

ResolvedPipelineSettings ResolvePipelineSettings(const AdvancedParserOptions &advanced);

/// Static-file TBB pipeline entry point. Stage A
/// `stageADriver(Token&) -> bool` is `serial_in_order`; Stage B
/// `stageBDecoder(Token, scratch, keys, columns, out)` runs in parallel.
/// Stage C coalescing, new-keys diff, inline timestamp promotion, and
/// stop_token cancellation are owned by this harness.
///
/// @p source is the static-file source the pipeline drives against.
/// Stage B's decoder uses `source.File()` for direct mmap access and
/// stamps each `LogLine` it builds with `&source` and the line's
/// absolute `lineId`. Stage C rebases per-batch `OwnedString` payloads
/// into `source.File()`'s session-global arena once per batch (single-
/// threaded write).
template <class Token, class UserState, class StageADriver, class StageBDecoder>
void RunStaticParserPipeline(
    FileLineSource &source,
    LogParseSink &sink,
    const ParserOptions &options,
    const AdvancedParserOptions &advanced,
    StageADriver &&stageADriver,
    StageBDecoder &&stageBDecoder
)
{
    LogFile &file = source.File();

    sink.OnStarted();

    KeyIndex &keys = sink.Keys();
    BatchCoalescer coalescer(sink, keys, kStaticBatchFlushLines, kStaticBatchFlushInterval);

    // Honour the `LogParseSink` contract: emit at least one (possibly
    // empty) `OnBatch` before `OnFinished` on every early-exit path.
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

        // Rebase per-batch `OwnedString` offsets onto the canonical
        // `LogFile` arena. Stage C is `serial_in_order`, so writing to
        // `file.AppendOwnedStrings` is single-threaded.
        if (!parsed.ownedStringsArena.empty())
        {
            const uint64_t delta = file.AppendOwnedStrings(parsed.ownedStringsArena);
            for (LogLine &line : parsed.lines)
            {
                line.RebaseOwnedStringOffsets(delta);
            }
        }

        // Stage B's `relativeLine` is 1-based within the batch. Compose the
        // absolute "Error on line N: ..." message here so the line-number
        // shift on each `LogLine`'s id and the line-number shown to the
        // user stay in lockstep across batches.
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

        if (prefersUncoalesced)
        {
            StreamedBatch out;
            out.lines = std::move(parsed.lines);
            out.localLineOffsets = std::move(parsed.localLineOffsets);
            formatErrorsInto(out.errors);
            out.firstLineNumber = nextLineNumber;
            coalescer.DrainNewKeysInto(out);

            nextLineNumber += parsed.totalLineCount;

            sink.OnBatch(std::move(out));
            return;
        }

        StreamedBatch &pending = coalescer.Pending();

        // Defer the prime until we have a real line: `firstLineNumber` is
        // documented (and treated by sinks) as the 1-based absolute line
        // number of the first source line in the batch -- *not* of the first
        // chunk that hit `pending`. Priming on an all-error / all-blank chunk
        // would set `firstLineNumber` to a value strictly less than
        // `lines.front().LineId()` once the next chunk's first real line
        // lands here.
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
    };

    oneapi::tbb::global_control gc(
        oneapi::tbb::global_control::max_allowed_parallelism, static_cast<size_t>(settings.effectiveThreads)
    );

    oneapi::tbb::parallel_pipeline(
        settings.ntokens,
        // A: serial_in_order -- drives Stage A tokens in source order.
        oneapi::tbb::make_filter<void, Token>(oneapi::tbb::filter_mode::serial_in_order, stageA) &
            // B: parallel -- decode batches concurrently.
            oneapi::tbb::make_filter<Token, ParsedPipelineBatch>(oneapi::tbb::filter_mode::parallel, stageB) &
            // C: serial_in_order -- coalesce / emit batches in source order.
            oneapi::tbb::make_filter<ParsedPipelineBatch, void>(oneapi::tbb::filter_mode::serial_in_order, stageC)
    );

    // Both modes converge on `BatchCoalescer::Finish`: in the
    // `prefersUncoalesced` mode the coalescer's pending stays empty so
    // it just emits one terminal `tail` batch primed at the next line
    // number. In coalesced mode it flushes whatever pending content
    // accumulated from trailing all-error / all-blank chunks (errors,
    // localLineOffsets, or new keys) and otherwise emits the same
    // primed-empty tail.
    coalescer.Finish(nextLineNumber, stopToken.stop_requested());
}

} // namespace loglib::internal
