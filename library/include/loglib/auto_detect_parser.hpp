#pragma once

#include "log_line.hpp"
#include "log_parser.hpp"
#include "parser_options.hpp"

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
/// option gets the same "same bytes -> same format" auto-detect
/// that static files enjoy via `DetectFormatForPath`. Also usable
/// on any other streaming input where the sender's format isn't
/// known up front (stdin's synchronous `StdinPeek` path already
/// resolves the format before creating the producer, so it does
/// not need this adapter, but a subclass could plug in one).
///
/// On the file path (`ParseStreaming(FileLineSource&, ...)`) this
/// class detects via `ReadProbeHead` on the source's path and
/// delegates. It is not intended to be selected for static file
/// opens (`MainWindow::DetectFormatForPath` handles those); the
/// file overload exists purely to satisfy the `LogParser`
/// interface for completeness / tests.
class AutoDetectParser : public LogParser
{
public:
    /// Peek budget: the first @p peekBytes bytes are drained from
    /// the producer to run the probe. Defaults to
    /// `PROBE_BYTES_BUDGET`, matching every other detector.
    explicit AutoDetectParser(std::size_t peekBytes = PROBE_BYTES_BUDGET) noexcept;

    /// Always returns `true`: the adapter accepts any bytes and
    /// lets the resolved parser make the final call. Kept virtual
    /// so `LogFactory::Create` (which wouldn't return this class
    /// anyway) still compiles when someone extends the enum.
    bool IsValidBytes(std::string_view sniffBuffer) const override;

    /// Detect the format from the file's head and delegate to the
    /// resolved parser. Sink starts under the resolved parser.
    void ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /// Drain up to `peekBytes` bytes from `source.Producer()`,
    /// detect the format, then invoke the resolved parser with
    /// `options.initialCarry = <drained bytes>`. Peek loop
    /// respects `options.stopToken` and honours the producer's
    /// `WaitForBytes` cadence, so it plays nicely with the
    /// existing streaming lifecycle. If the peek is empty (stop
    /// before any bytes; producer closed before yielding), the
    /// adapter delegates to `JsonParser` -- which will produce a
    /// clean empty session under an empty stopped producer.
    void ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options = {}) const override;

    /// Delegates to `JsonParser::ToString`, which just serialises
    /// a `KV` map. `Edit -> Copy` uses `LineSource::RawLine`
    /// anyway; this override matters only when the GUI has no
    /// resolved parser handy (test-only path).
    std::string ToString(const LogLine &line) const override;

private:
    std::size_t mPeekBytes;
};

} // namespace loglib
