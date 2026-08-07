#include "loglib/auto_detect_parser.hpp"

#include "loglib/bytes_producer.hpp"
#include "loglib/file_line_source.hpp"
#include "loglib/format_detection.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_parser.hpp"
#include "loglib/parser_options.hpp"
#include "loglib/parsers/json_parser.hpp"
#include "loglib/stream_line_source.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <memory>
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

std::string DrainPeek(BytesProducer &producer, std::size_t peekBudget, const StopToken &stopToken)
{
    std::string peek;
    peek.reserve(peekBudget);
    std::vector<char> chunk(PEEK_CHUNK_BYTES);
    while (peek.size() < peekBudget && !stopToken.stop_requested())
    {
        const std::size_t want = std::min(chunk.size(), peekBudget - peek.size());
        const std::size_t got = producer.Read(std::span<char>(chunk.data(), want));
        if (got != 0)
        {
            peek.append(chunk.data(), got);
            continue;
        }
        if (producer.IsClosed())
        {
            break;
        }
        producer.WaitForBytes(PEEK_WAIT_INTERVAL);
    }
    return peek;
}

} // namespace

AutoDetectParser::AutoDetectParser(std::size_t peekBytes) noexcept : mPeekBytes(peekBytes) {}

bool AutoDetectParser::IsValidBytes(std::string_view) const
{
    return true;
}

void AutoDetectParser::ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    // Static-file detection is fully served by `DetectFormatForPath`,
    // which reads the head bytes once. Route through it so we don't
    // double-read the file just to satisfy the interface.
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

    std::string peek = DrainPeek(*producer, mPeekBytes, options.stopToken);

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
        // Nothing to detect on: either the caller cancelled before
        // any bytes arrived, or the producer closed empty. Delegate
        // to `JsonParser` so the streaming loop emits its terminal
        // batch cleanly.
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
