#include "loglib/parse/auto_detect_parser.hpp"

#include "loglib/core/bytes_producer.hpp"
#include "loglib/core/file_line_source.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/stream_line_source.hpp"
#include "loglib/parse/format_detection.hpp"
#include "loglib/parse/log_parser.hpp"
#include "loglib/parse/parser_options.hpp"
#include "loglib/parse/parsers/json_parser.hpp"

#include <algorithm>
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

/**
 * Peek chunk size. Small enough to keep memory footprint tight
 * while draining the probe budget; large enough that the syscall
 * overhead vanishes next to WaitForBytes.
 */
constexpr std::size_t PEEK_CHUNK_BYTES = 4096;

/**
 * How long the peek loop parks in WaitForBytes between
 * non-productive Read calls. Matches the streaming loop's
 * STREAMING_BATCH_FLUSH_INTERVAL so the adapter is
 * indistinguishable from a normal parser in profile traces.
 */
constexpr auto PEEK_WAIT_INTERVAL = std::chrono::milliseconds(100);

std::string DrainPeek(
    BytesProducer &producer,
    std::size_t peekBudget,
    std::chrono::milliseconds peekTimeout,
    const StopToken &stopToken,
    const RegexTemplateCatalog &catalog
)
{
    std::string peek;
    peek.reserve(peekBudget);
    std::vector<char> chunk(PEEK_CHUNK_BYTES);
    const bool enforceDeadline = peekTimeout > std::chrono::milliseconds::zero();
    // The deadline starts with the first byte, not with the peek:
    // a network listener is typically started well before its
    // senders connect, and an idle listener carries no format
    // evidence. Until data arrives, wait for stop or close only.
    std::optional<std::chrono::steady_clock::time_point> deadline;
    while (peek.size() < peekBudget && !stopToken.StopRequested())
    {
        const std::size_t want = std::min(chunk.size(), peekBudget - peek.size());
        const std::size_t got = producer.Read(std::span<char>(chunk.data(), want));
        if (got != 0)
        {
            if (!deadline)
            {
                deadline = std::chrono::steady_clock::now() + peekTimeout;
            }
            peek.append(chunk.data(), got);
            // Early exit: DetectFormat only returns a
            // value when a probe actually claimed the buffer. Once
            // it commits, reading further bytes cannot reverse the
            // verdict, so hand off to the resolved parser without
            // waiting for the peek to fill or the deadline to
            // elapse. Gate on "there is at least one newline" so
            // we don't run detection on partial first lines that
            // JSON/logfmt would rightly reject.
            if (peek.contains('\n') && DetectFormat(peek, catalog).has_value())
            {
                break;
            }
            continue;
        }
        if (producer.IsClosed())
        {
            break;
        }
        if (enforceDeadline && deadline)
        {
            const auto now = std::chrono::steady_clock::now();
            if (now >= *deadline)
            {
                break;
            }
            // Clamp WaitForBytes at whatever's left of the
            // deadline so we don't overshoot by up to
            // PEEK_WAIT_INTERVAL. duration_cast truncates
            // toward zero; guard against a zero-duration park
            // by breaking out instead.
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - now);
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

AutoDetectParser::AutoDetectParser(
    std::size_t peekBytes, std::chrono::milliseconds peekTimeout, bool logfmtMultiline, RegexTemplateCatalog catalog
)
    : mPeekBytes(peekBytes), mPeekTimeout(peekTimeout), mLogfmtMultiline(logfmtMultiline), mCatalog(std::move(catalog))
{
}

void AutoDetectParser::Parse(FileLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    // Static files are detected from their head bytes, which are read once.
    FormatSpec detected = DetectFormat(ReadProbeHead(source.Path()), mCatalog).value_or(FormatSpec{});
    detected.logfmtMultiline = mLogfmtMultiline;
    const std::unique_ptr<LogParser> resolved = MakeParser(detected);
    resolved->Parse(source, sink, std::move(options));
}

void AutoDetectParser::Parse(StreamLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    BytesProducer *producer = source.Producer();
    if (producer == nullptr)
    {
        // No producer -- fall through to Json's streaming loop so
        // the sink still honours its OnStarted / terminal-batch
        // contract (see RunStreamingParseLoop).
        JsonParser().Parse(source, sink, std::move(options));
        return;
    }

    // Bytes the caller gave back to the source come first, then the peek, so the detector sees the head bytes that the
    // parser will see.
    std::string combined = source.TakeUnreadBytes();
    combined.append(DrainPeek(*producer, mPeekBytes, mPeekTimeout, options.stopToken, mCatalog));

    if (combined.empty())
    {
        // Stop, timeout, or EOF may leave no bytes to detect.
        // Delegate to JSON so the normal sink lifecycle still runs.
        JsonParser().Parse(source, sink, std::move(options));
        return;
    }

    FormatSpec detected = DetectFormat(combined, mCatalog).value_or(FormatSpec{});
    detected.logfmtMultiline = mLogfmtMultiline;
    const std::unique_ptr<LogParser> resolved = MakeParser(detected);
    source.UnreadBytes(std::move(combined));
    resolved->Parse(source, sink, std::move(options));
}

} // namespace loglib
