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
struct StreamedBatch
{
    std::vector<LogLine> lines;
    std::vector<uint64_t> localLineOffsets;
    std::vector<std::string> errors;
    std::vector<std::string> newKeys;
    /// 1-based absolute line number of `lines.front()`.
    size_t firstLineNumber = 0;
};

/// Sink interface for the streaming log parser. Methods are called from a
/// single serial-in-order worker, in this order:
///   1. exactly one `OnStarted()`,
///   2. zero or more `OnBatch(...)` (always at least one, possibly empty, before finish),
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
    virtual bool PrefersUncoalesced() const
    {
        return false;
    }
};

} // namespace loglib
