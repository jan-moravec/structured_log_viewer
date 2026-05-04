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

#include <oneapi/tbb/enumerable_thread_specific.h>
#include <oneapi/tbb/global_control.h>
#include <oneapi/tbb/parallel_pipeline.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
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
};

/// Resolved defaults for `effectiveThreads` and `ntokens`. Both >= 1.
struct ResolvedPipelineSettings
{
    unsigned int effectiveThreads = 1;
    size_t ntokens = 0;
};

ResolvedPipelineSettings ResolvePipelineSettings(const AdvancedParserOptions &advanced);

/// Static-file TBB pipeline. Stage A (`serial_in_order`) drives
/// tokens; Stage B (`parallel`) decodes; Stage C (`serial_in_order`)
/// rebases per-batch arenas into `source.File()`'s session-global
/// arena, coalesces, diffs new keys, runs inline timestamp promotion,
/// and honours `stop_token`. Stage B stamps each emitted `LogLine` with
/// `&source` and its absolute `lineId`.
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
    BatchCoalescer coalescer(sink, keys, STATIC_BATCH_FLUSH_LINES, STATIC_BATCH_FLUSH_INTERVAL);

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

    coalescer.Finish(nextLineNumber, stopToken.stop_requested());
}

} // namespace loglib::internal
