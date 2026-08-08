#include "loglib/auto_detect_parser.hpp"

#include "loglib/bytes_producer.hpp"
#include "loglib/file_line_source.hpp"
#include "loglib/format_detection.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_parser.hpp"
#include "loglib/parser_options.hpp"
#include "loglib/parsers/json_parser.hpp"
#include "loglib/stream_line_source.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

/// Peek chunk size. Small enough to keep memory footprint tight
/// while draining the probe budget; large enough that the syscall
/// overhead vanishes next to `WaitForBytes`.
constexpr std::size_t PEEK_CHUNK_BYTES = 4096;

/// How long the peek loop parks in `WaitForBytes` between
/// non-productive `Read` calls. Matches the streaming loop's
/// `STREAMING_BATCH_FLUSH_INTERVAL` so the adapter is
/// indistinguishable from a normal parser in profile traces.
constexpr auto PEEK_WAIT_INTERVAL = std::chrono::milliseconds(100);

std::string DrainPeek(
    BytesProducer &producer, std::size_t peekBudget, std::chrono::milliseconds peekTimeout, const StopToken &stopToken
)
{
    std::string peek;
    peek.reserve(peekBudget);
    std::vector<char> chunk(PEEK_CHUNK_BYTES);
    const bool enforceDeadline = peekTimeout > std::chrono::milliseconds::zero();
    const auto deadline = std::chrono::steady_clock::now() + peekTimeout;
    while (peek.size() < peekBudget && !stopToken.stop_requested())
    {
        const std::size_t want = std::min(chunk.size(), peekBudget - peek.size());
        const std::size_t got = producer.Read(std::span<char>(chunk.data(), want));
        if (got != 0)
        {
            peek.append(chunk.data(), got);
            // Early exit: `TryDetectFormatFromBytes` only returns a
            // value when a probe actually claimed the buffer. Once
            // it commits, reading further bytes cannot reverse the
            // verdict, so hand off to the resolved parser without
            // waiting for the peek to fill or the deadline to
            // elapse. Gate on "there is at least one newline" so
            // we don't run detection on partial first lines that
            // JSON/logfmt would rightly reject.
            if (peek.contains('\n') && TryDetectFormatFromBytes(peek).has_value())
            {
                break;
            }
            continue;
        }
        if (producer.IsClosed())
        {
            break;
        }
        if (enforceDeadline)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline)
            {
                break;
            }
            // Clamp `WaitForBytes` at whatever's left of the
            // deadline so we don't overshoot by up to
            // `PEEK_WAIT_INTERVAL`. `duration_cast` truncates
            // toward zero; guard against a zero-duration park
            // by breaking out instead.
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
            const auto waitFor = std::min(remaining, PEEK_WAIT_INTERVAL);
            if (waitFor <= std::chrono::milliseconds::zero())
            {
                break;
            }
            producer.WaitForBytes(waitFor);
            continue;
        }
        producer.WaitForBytes(PEEK_WAIT_INTERVAL);
    }
    return peek;
}

} // namespace

AutoDetectParser::AutoDetectParser(std::size_t peekBytes, std::chrono::milliseconds peekTimeout) noexcept
    : mPeekBytes(peekBytes), mPeekTimeout(peekTimeout)
{
}

bool AutoDetectParser::IsValidBytes(std::string_view /*sniffBuffer*/) const
{
    return true;
}

void AutoDetectParser::ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    // Static-file detection is fully served by `DetectFormatForPath`,
    // which reads the head bytes once. Route through it so we don't
    // double-read the file just to satisfy the interface.
    //
    // `initialCarry` is a streaming-only mechanism (see
    // `parser_options.hpp` and the invariant asserted by
    // `RunStaticParserPipeline`): a static file parse reads from
    // the mmap directly and has nowhere to splice the carry in.
    // Guard the invariant here so a misconfigured caller trips a
    // debug-only assert with a clear message instead of aborting
    // deep inside the pipeline, and clear the carry in release so
    // the delegate does not inherit garbage.
    assert(
        options.initialCarry.empty() &&
        "AutoDetectParser::ParseStreaming(FileLineSource, ...) does not support initialCarry; use the "
        "StreamLineSource overload"
    );
    options.initialCarry.clear();
    const DetectedFormat detected = DetectFormatForPath(source.Path());
    const std::unique_ptr<LogParser> resolved = MakeParserForFormat(detected.format, detected.regexPattern);
    resolved->ParseStreaming(source, sink, std::move(options));
}

void AutoDetectParser::ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    BytesProducer *producer = source.Producer();
    if (producer == nullptr)
    {
        // No producer -- fall through to Json's streaming loop so
        // the sink still honours its `OnStarted` / terminal-batch
        // contract (see `RunStreamingParseLoop`).
        JsonParser().ParseStreaming(source, sink, std::move(options));
        return;
    }

    std::string peek = DrainPeek(*producer, mPeekBytes, mPeekTimeout, options.stopToken);

    // Caller-supplied `initialCarry` semantically prefixes the
    // producer's stream (bytes already consumed by an earlier
    // stage). Combine them for both detection and delivery so the
    // resolved parser sees `<caller carry> || <peek> || <future
    // producer bytes>` in-order, and so the detector inspects the
    // same head bytes the parser will see.
    std::string combined;
    combined.reserve(options.initialCarry.size() + peek.size());
    combined.append(options.initialCarry);
    combined.append(peek);
    options.initialCarry.clear();

    if (combined.empty())
    {
        // Stop, timeout, or EOF may leave no bytes to detect.
        // Delegate to JSON so the normal sink lifecycle still runs.
        JsonParser().ParseStreaming(source, sink, std::move(options));
        return;
    }

    const DetectedFormat detected = DetectFormatFromBytes(combined);
    const std::unique_ptr<LogParser> resolved = MakeParserForFormat(detected.format, detected.regexPattern);
    options.initialCarry = std::move(combined);
    resolved->ParseStreaming(source, sink, std::move(options));
}

std::string AutoDetectParser::ToString(const LogLine &line) const
{
    return JsonParser().ToString(line);
}

} // namespace loglib
