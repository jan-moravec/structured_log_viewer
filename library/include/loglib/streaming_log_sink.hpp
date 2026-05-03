#pragma once

#include "key_index.hpp"
#include "log_line.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace loglib
{

/// One unit of work handed from the streaming parser to a `StreamingLogSink`.
/// A "rows-empty" batch with non-empty `errors`/`newKeys` is valid; the parser
/// always emits a final batch before `OnFinished`.
///
/// `lines` carries every parsed log row regardless of session type
/// (static file or live tail): each `LogLine` is tagged with its
/// `LineSource *` so resolution stays uniform. `localLineOffsets` is
/// only populated by the file-source pipeline and is forwarded to the
/// `LogFile`'s line-offset table; the streaming pipeline leaves it
/// empty.
struct StreamedBatch
{
    std::vector<LogLine> lines;
    std::vector<uint64_t> localLineOffsets;
    std::vector<std::string> errors;
    std::vector<std::string> newKeys;
    /// 1-based absolute line number of the batch's start cursor.
    /// - When `lines` is non-empty: matches the chunk start, not necessarily
    ///   the first parsed line (errors preceding it can push it lower).
    /// - When `lines` is empty: the line cursor at the time the batch was
    ///   sealed; not tied to any specific source line.
    size_t firstLineNumber = 0;
};

/// Sink interface for the streaming log parser. Methods are called from a
/// single serial-in-order worker, in this order:
///   1. exactly one `OnStarted()`,
///   2. **at least one** `OnBatch(...)` — the harness *always* emits a
///      terminal batch (possibly empty, with `lines.empty() &&
///      errors.empty() && newKeys.empty()`) before `OnFinished`, so sinks
///      that lazily initialise on first `OnBatch` work uniformly with
///      empty-source / cancelled-before-Stage-A parses,
///   3. exactly one `OnFinished(cancelled)` — `cancelled == true` if the
///      parse was stopped via the `ParserOptions::stopToken`.
class StreamingLogSink
{
public:
    virtual ~StreamingLogSink() = default;

    /// Canonical `KeyIndex` the parser interns keys into. Accessed concurrently
    /// from every worker; must remain stable between `OnStarted` and `OnFinished`.
    virtual KeyIndex &Keys() = 0;

    virtual void OnStarted() = 0;

    virtual void OnBatch(StreamedBatch batch) = 0;

    virtual void OnFinished(bool cancelled) = 0;

    /// When true, the harness forwards each pipeline batch straight to
    /// `OnBatch` without GUI-style coalescing. Sinks that already buffer
    /// internally (e.g. `BufferingSink`) opt in.
    [[nodiscard]] virtual bool PrefersUncoalesced() const noexcept
    {
        return false;
    }
};

} // namespace loglib
