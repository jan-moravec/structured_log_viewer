#pragma once

#include "log_line.hpp"
#include "log_parser.hpp"
#include "parser_options.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>

namespace loglib
{

class FileLineSource;
class LogParseSink;
class StreamLineSource;

/// Adapter parser that drains a peek buffer from the producer,
/// runs the shared `DetectFormatFromBytes` probe, and delegates
/// to the resolved parser via `ParserOptions::initialCarry` so no
/// bytes are lost.
///
/// Purpose: the Network Stream dialog's `Format::AutoDetect`
/// option gets the same detection behaviour as static files. It is
/// also usable for other streaming inputs whose format is unknown.
///
/// On the file path (`ParseStreaming(FileLineSource&, ...)`) this
/// class detects via `ReadProbeHead` on the source's path and
/// delegates. It is not intended to be selected for static file
/// opens (callers use `DetectFormatForPath` directly); the
/// file overload exists purely to satisfy the `LogParser`
/// interface for completeness / tests.
class AutoDetectParser : public LogParser
{
public:
    /// Default wall-clock budget for the streaming peek. Matches
    /// `StdinPeek`'s timeout so a bursty producer that hasn't yet
    /// delivered enough bytes to disambiguate its format still
    /// hands control to the resolved parser within a bounded
    /// interval instead of blocking indefinitely on `WaitForBytes`.
    static constexpr auto DEFAULT_PEEK_TIMEOUT = std::chrono::milliseconds(500);

    /// @param peekBytes    Number of bytes drained from the producer
    ///                     to run the probe. Defaults to
    ///                     `PROBE_BYTES_BUDGET`, matching every
    ///                     other detector.
    /// @param peekTimeout  Wall-clock deadline for the peek loop.
    ///                     Zero or negative disables the deadline
    ///                     (used by tests that want to drain the
    ///                     full producer synchronously). Detection
    ///                     also short-circuits before the deadline
    ///                     once the buffer contains enough bytes
    ///                     for `TryDetectFormatFromBytes` to
    ///                     commit to a verdict.
    explicit AutoDetectParser(
        std::size_t peekBytes = PROBE_BYTES_BUDGET, std::chrono::milliseconds peekTimeout = DEFAULT_PEEK_TIMEOUT
    ) noexcept;

    /// Always returns `true`; format selection happens in
    /// `ParseStreaming`, not in `IsValidBytes`.
    bool IsValidBytes(std::string_view sniffBuffer) const override;

    /// Detect the format from the file's head and delegate to the
    /// resolved parser. Sink starts under the resolved parser.
    void ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /// Combine any existing `options.initialCarry` with up to
    /// `peekBytes` bytes from `source.Producer()`, detect the format,
    /// and pass that combined prefix to the resolved parser. The peek loop
    /// respects `options.stopToken` and honours the producer's
    /// `WaitForBytes` cadence, so it plays nicely with the
    /// existing streaming lifecycle. If no bytes are available after
    /// stop, timeout, or EOF, `JsonParser` preserves the sink lifecycle.
    void ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /// Delegates to `JsonParser::ToString`, which just serialises
    /// a `KV` map. `Edit -> Copy` uses `LineSource::RawLine`
    /// anyway; this override matters only when the GUI has no
    /// resolved parser handy (test-only path).
    std::string ToString(const LogLine &line) const override;

private:
    std::size_t mPeekBytes;
    std::chrono::milliseconds mPeekTimeout;
};

} // namespace loglib
