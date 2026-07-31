#pragma once

#include "key_index.hpp"
#include "log_line.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace loglib
{

/// Span of physical lines making up one multi-line record; the header
/// line owns the span. Both indices are 0-based absolute physical-line
/// indices (i.e. indices into `LogFile::mLineOffsets` once
/// `localLineOffsets` has been appended). The static parser attaches
/// one entry per multi-line record it seals; consumers register it via
/// `LogFile::RegisterMultiLineRecord` *after* `AppendLineOffsets` so
/// `GetLine(headerLineId)` can look up the terminating offset in the
/// same append batch.
struct MultiLineRecordSpan
{
    size_t headerLineId = 0;
    size_t lastLineId = 0;
};

/// One unit of work handed from the parser to a `LogParseSink`. A
/// "rows-empty" batch with non-empty `errors`/`newKeys` is valid; the parser
/// always emits a final batch before `OnFinished`.
///
/// `lines` carries every parsed log row regardless of session type (static
/// file or live tail): each `LogLine` is tagged with its `LineSource *` so
/// resolution stays uniform. `localLineOffsets` is only populated by the
/// file-source pipeline and is forwarded to the `LogFile`'s line-offset
/// table; the live-tail loop leaves it empty.
struct StreamedBatch
{
    std::vector<LogLine> lines;
    std::vector<uint64_t> localLineOffsets;
    std::vector<std::string> errors;
    std::vector<std::string> newKeys;
    /// Multi-line record spans (header + last physical line) that
    /// belong to this batch. The consumer *must* register these on
    /// `LogFile` **after** `AppendLineOffsets`, otherwise
    /// `GetLine(headerLineId)` would look up a stop offset that has
    /// not yet been appended and silently return only the header
    /// line's bytes. Populated only by the static parser pipeline
    /// when a template runs in multi-line mode; the live-tail loop
    /// joins bytes directly on the `StreamLineSource` and leaves
    /// this empty.
    std::vector<MultiLineRecordSpan> multiLineSpans;
    /// 1-based absolute line number of the batch's start cursor.
    /// - When `lines` is non-empty: matches the chunk start, not necessarily
    ///   the first parsed line (errors preceding it can push it lower).
    /// - When `lines` is empty: the line cursor at the time the batch was
    ///   sealed; not tied to any specific source line.
    size_t firstLineNumber = 0;
};

/// Sink interface for the log parser. Methods are called from a single
/// serial-in-order worker, in this order:
///   1. exactly one `OnStarted()`,
///   2. **at least one** `OnBatch(...)` -- the parser *always* emits a
///      terminal batch (possibly empty, with `lines.empty() &&
///      errors.empty() && newKeys.empty()`) before `OnFinished`, so sinks
///      that lazily initialise on first `OnBatch` work uniformly with
///      empty-source / cancelled-before-Stage-A parses,
///   3. exactly one `OnFinished(cancelled)` -- `cancelled == true` if the
///      parse was stopped via the `ParserOptions::stopToken`.
///
/// The same interface serves both the synchronous static-file path
/// (`BufferingSink`) and the live-tail GUI path (`QtStreamingLogSink`).
class LogParseSink
{
public:
    virtual ~LogParseSink() = default;

    /// Canonical `KeyIndex` the parser interns keys into. Accessed concurrently
    /// from every worker; must remain stable between `OnStarted` and `OnFinished`.
    virtual KeyIndex &Keys() = 0;

    virtual void OnStarted() = 0;

    virtual void OnBatch(StreamedBatch batch) = 0;

    virtual void OnFinished(bool cancelled) = 0;

    /// When true, the parser forwards each pipeline batch straight to
    /// `OnBatch` without GUI-style coalescing. Sinks that already buffer
    /// internally (e.g. `BufferingSink`) opt in.
    [[nodiscard]] virtual bool PrefersUncoalesced() const noexcept
    {
        return false;
    }
};

} // namespace loglib
