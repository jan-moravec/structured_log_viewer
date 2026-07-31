#pragma once

#include "loglib/bytes_producer.hpp"
#include "loglib/internal/batch_coalescer.hpp"
#include "loglib/internal/compact_log_value.hpp"
#include "loglib/internal/line_decoder.hpp"
#include "loglib/internal/parse_runtime.hpp"
#include "loglib/internal/timestamp_promotion.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_parse_sink.hpp"
#include "loglib/parser_options.hpp"
#include "loglib/stop_token.hpp"
#include "loglib/stream_line_source.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib::internal
{

/// Coalescing thresholds for the live-tail loop. Tighter than the
/// static pipeline because we optimise for latency, not throughput.
constexpr size_t STREAMING_BATCH_FLUSH_LINES = 250;
constexpr auto STREAMING_BATCH_FLUSH_INTERVAL = std::chrono::milliseconds(100);

/// Max age of a not-yet-committed multi-line record. If a decoder
/// opens a record with `Emit` and then only ships `Continue`s (or
/// nothing) for this long, the loop commits the record with the
/// continuation bytes it has so far and starts fresh on the next
/// `Emit`. Longer than `STREAMING_BATCH_FLUSH_INTERVAL` so the batch
/// coalescer's cadence doesn't accidentally coalesce a pending
/// record on its own timer.
constexpr auto STREAMING_PENDING_RECORD_MAX_AGE = std::chrono::milliseconds(500);

/// Read buffer size for the live-tail loop. Matches `TailingBytesProducer`'s
/// pre-fill chunk; small enough that each `Read` returns promptly.
constexpr size_t STREAMING_READ_BUFFER_SIZE = 64 * 1024;

/// Extend @p targetKey's `OwnedString` bytes inside @p ownedArena with
/// @p continuationBytes (already prefixed with any needed separators).
/// Returns true if the field was found and extended, false otherwise
/// (unknown target, or target is a non-string type — orphan case).
///
/// `mmapBytes` is the memory-mapped file view (only meaningful for
/// the static pipeline where zero-copy `MmapSlice` values can appear).
/// Streaming callers pass an empty span; `MmapSlice` never appears in
/// the streaming path but is handled defensively via re-materialise
/// into the arena tail when the mmap view is available.
///
/// `Monostate` (a bare `key=` with no value) is promoted to
/// `OwnedString` in-place, minus the would-be leading '\n' separator.
inline bool ExtendContinuationTarget(
    std::vector<std::pair<KeyId, CompactLogValue>> &values, std::string &ownedArena, KeyId targetKey,
    std::string_view continuationBytes, std::string_view mmapBytes = std::string_view{}
)
{
    if (targetKey == INVALID_KEY_ID || continuationBytes.empty())
    {
        return targetKey != INVALID_KEY_ID; // empty continuation on a known target is a no-op success
    }

    for (auto &kv : values)
    {
        if (kv.first != targetKey)
        {
            continue;
        }
        CompactLogValue &v = kv.second;
        switch (v.tag)
        {
            case CompactTag::OwnedString:
            {
                const uint64_t offset = v.payload;
                const uint32_t length = v.aux;
                if (offset + length == ownedArena.size())
                {
                    // Target is at the arena tail — cheap append.
                    ownedArena.append(continuationBytes.data(), continuationBytes.size());
                    v.aux = static_cast<uint32_t>(static_cast<size_t>(length) + continuationBytes.size());
                }
                else
                {
                    // Target sits in the middle (rare; possible if a
                    // future decoder re-orders arena writes). Copy
                    // old + new to the tail and rebase. Reserve up
                    // front so neither `append` reallocates -- the
                    // first append reads from `ownedArena.data()`,
                    // which would dangle if the same call caused a
                    // grow (self-append into `std::string` is not
                    // guaranteed overlap-safe by the standard).
                    ownedArena.reserve(ownedArena.size() + length + continuationBytes.size());
                    const uint64_t newOffset = ownedArena.size();
                    ownedArena.append(ownedArena.data() + offset, length);
                    ownedArena.append(continuationBytes.data(), continuationBytes.size());
                    v.payload = newOffset;
                    v.aux = static_cast<uint32_t>(static_cast<size_t>(length) + continuationBytes.size());
                }
                return true;
            }
            case CompactTag::Monostate:
            {
                // Promote to OwnedString anchored at the arena tail.
                // Skip a would-be leading separator so an empty
                // `key=` grows into the continuation text directly
                // rather than starting with '\n'.
                std::string_view bytes = continuationBytes;
                if (!bytes.empty() && bytes.front() == '\n')
                {
                    bytes.remove_prefix(1);
                }
                v.tag = CompactTag::OwnedString;
                v.payload = ownedArena.size();
                v.aux = static_cast<uint32_t>(bytes.size());
                ownedArena.append(bytes.data(), bytes.size());
                return true;
            }
            case CompactTag::MmapSlice:
            {
                // Static pipeline: zero-copy `MmapSlice` values point
                // into the file mmap. Promote to `OwnedString` at the
                // arena tail by copying the original bytes plus the
                // continuation. Requires the file view (`mmapBytes`);
                // streaming callers pass empty and this becomes an
                // orphan-continuation signal.
                const uint64_t offset = v.payload;
                const uint32_t length = v.aux;
                if (mmapBytes.empty() || offset + length > mmapBytes.size())
                {
                    return false;
                }
                const uint64_t newOffset = ownedArena.size();
                ownedArena.append(mmapBytes.data() + offset, length);
                ownedArena.append(continuationBytes.data(), continuationBytes.size());
                v.tag = CompactTag::OwnedString;
                v.payload = newOffset;
                v.aux = static_cast<uint32_t>(static_cast<size_t>(length) + continuationBytes.size());
                return true;
            }
            case CompactTag::DictRef:
            case CompactTag::Int64:
            case CompactTag::Uint64:
            case CompactTag::Double:
            case CompactTag::Bool:
            case CompactTag::Timestamp:
                // Non-string target: can't splice text into it
                // without corrupting the type. Template validation
                // should have rejected this at load-time; if it
                // reaches here, treat as orphan.
                return false;
        }
        return false;
    }
    return false; // Target key not present in the values.
}

/// Format-agnostic live-tail entry point. Drains `source.Producer()`
/// line-by-line, hands each non-blank line to @p decoder (must
/// satisfy `CompactLineDecoder`), commits `(rawText, ownedArena)` to
/// @p source, and emits `LogLine`s into batches throttled by
/// `STREAMING_BATCH_FLUSH_*`.
///
/// Single-threaded: the target is thousands of lines/s, so TBB
/// overhead is not warranted. `source` is mutated on the parser
/// thread and read concurrently by the GUI; `StreamLineSource`'s
/// internal mutex + deque storage make that safe.
///
/// Multi-line records: if the decoder returns
/// `LineDecodeResult::Continue`, the loop holds the prior record
/// open (deferred emit), appending each continuation line's bytes
/// to the record's raw text (so `RawLine(lineId)` naturally sees
/// the joined text) and to the last-source-order field via
/// `decoder.LastContinuationTarget()` (only inspected when the
/// decoder exposes it — JSON / CSV never return `Continue`, so
/// they don't need to opt in). The pending record commits on the
/// next `Emit`, on EOF, or after `STREAMING_PENDING_RECORD_MAX_AGE`
/// (so hung producers don't stall a partial trace forever).
///
/// @p newKeyBaseline forwards to `BatchCoalescer`. Parsers that
/// pre-intern keys into the sink's `KeyIndex` before calling here
/// (e.g. `RegexParser`, whose schema comes from the pattern's
/// named capture groups) pass the snapshot taken *before* the
/// intern step, so eagerly-interned columns still surface as
/// `newKeys` on the first emitted batch. Parsers that add keys
/// lazily (JSON, logfmt, CSV) leave this `std::nullopt`.
template <class Decoder>
void RunStreamingParseLoop(
    StreamLineSource &source,
    Decoder &decoder,
    LogParseSink &sink,
    const ParserOptions &options,
    std::optional<size_t> newKeyBaseline = std::nullopt
)
{
    sink.OnStarted();

    KeyIndex &keys = sink.Keys();
    BatchCoalescer coalescer(sink, keys, STREAMING_BATCH_FLUSH_LINES, STREAMING_BATCH_FLUSH_INTERVAL, newKeyBaseline);

    BytesProducer *producer = source.Producer();
    if (producer == nullptr)
    {
        // No producer (e.g. unit tests driving `AppendLine` directly).
        // Honour the sink contract with one empty terminal batch.
        coalescer.Finish(1, false);
        return;
    }

    const StopToken stopToken = options.stopToken;

    const std::vector<TimeColumnSpec> timeColumns = BuildTimeColumnSpecs(keys, options.configuration.get());
    std::span<const TimeColumnSpec> timeColumnsSpan(timeColumns);

    WorkerScratchBase promoteScratch;
    promoteScratch.EnsureTimeColumnCapacity(timeColumns.size());

    size_t nextLineNumber = 1;

    std::string carry;
    std::vector<char> readBuffer(STREAMING_READ_BUFFER_SIZE);

    // Reused per line; move-transferred into the source on success.
    std::vector<std::pair<KeyId, CompactLogValue>> compactValues;
    std::string ownedArena;
    std::string lineError;

    /// Not-yet-committed record awaiting either a fresh header
    /// (`Emit`), EOF, or a pending-age timeout. Held here rather than
    /// inside `BatchCoalescer::Pending()` because the record's
    /// `LogLine` isn't stable until commit — its `lineId` allocation
    /// happens on `AppendLine`, which we haven't called yet.
    struct PendingRecord
    {
        std::string rawText;    // header line's bytes; continuations get appended
                                // on commit so `RawLine(lineId)` returns the joined form.
        std::string ownedArena; // header's decoded field bytes.
        std::vector<std::pair<KeyId, CompactLogValue>> compactValues;
        size_t startLineNumber = 0; // physical line number of the header.
        KeyId continuationTarget = INVALID_KEY_ID;
        std::string continuationBytes; // "\ncont1\ncont2" (starts with '\n' when non-empty).
        std::chrono::steady_clock::time_point openedAt;
    };
    std::optional<PendingRecord> pending;

    // Consecutive orphan-continuation lines (a stack trace pasted at
    // the top of a stream, a broken chunk that starts mid-record,
    // etc.) fold into one summary error per run instead of one
    // error per physical line -- N lines used to produce N Error-
    // list entries and drown out real parse errors.
    size_t orphanRunFirstLine = 0;
    size_t orphanRunLastLine = 0;
    auto flushOrphanRun = [&]() {
        if (orphanRunFirstLine == 0)
        {
            return;
        }
        std::string msg;
        if (orphanRunFirstLine == orphanRunLastLine)
        {
            msg = fmt::format("Error on line {}: Orphaned continuation line.", orphanRunFirstLine);
        }
        else
        {
            msg = fmt::format(
                "Error on lines {}-{}: {} orphaned continuation lines dropped.",
                orphanRunFirstLine,
                orphanRunLastLine,
                orphanRunLastLine - orphanRunFirstLine + 1
            );
        }
        coalescer.Pending().errors.emplace_back(std::move(msg));
        orphanRunFirstLine = 0;
        orphanRunLastLine = 0;
    };

    auto commitPending = [&]() {
        if (!pending)
        {
            return;
        }
        PendingRecord rec = std::move(*pending);
        pending.reset();

        if (!rec.continuationBytes.empty())
        {
            const bool extended =
                ExtendContinuationTarget(rec.compactValues, rec.ownedArena, rec.continuationTarget, rec.continuationBytes);
            if (extended)
            {
                rec.rawText.append(rec.continuationBytes);
            }
            else
            {
                // Target field can't accept text (e.g. typed value,
                // MmapSlice fallback, or key not present). Surface
                // one error per orphaned run — the header record
                // still emits cleanly with just its own bytes.
                coalescer.Pending().errors.emplace_back(fmt::format(
                    "Continuation lines dropped on line {}: target field is not a string.", rec.startLineNumber
                ));
            }
        }

        std::sort(rec.compactValues.begin(), rec.compactValues.end(), [](const auto &a, const auto &b) {
            return a.first < b.first;
        });

        const size_t lineId = source.AppendLine(std::move(rec.rawText), std::move(rec.ownedArena));
        LogLine logLine(std::move(rec.compactValues), keys, source, lineId);
        // Promote once on the fully-joined record so timestamps
        // aren't re-parsed and continuation bytes on a typo'd
        // timestamp column don't corrupt the parsed value.
        promoteScratch.PromoteTimestamps(logLine, timeColumnsSpan, std::string_view{});
        coalescer.Prime(rec.startLineNumber);
        coalescer.Pending().lines.push_back(std::move(logLine));
    };

    auto processLine = [&](std::string_view line) {
        std::string_view trimmed = line;
        if (!trimmed.empty() && trimmed.back() == '\r')
        {
            trimmed.remove_suffix(1);
        }
        const size_t lineNumber = nextLineNumber;
        ++nextLineNumber;

        if (trimmed.empty())
        {
            flushOrphanRun();
            return;
        }

        const LineDecodeResult result =
            decoder.DecodeCompact(trimmed, keys, &promoteScratch.keyCache, compactValues, ownedArena, lineError);
        if (result == LineDecodeResult::Error)
        {
            flushOrphanRun();
            coalescer.Pending().errors.emplace_back(
                fmt::format("Error on line {}: {}", lineNumber, std::move(lineError))
            );
            return;
        }
        if (result == LineDecodeResult::Skip)
        {
            flushOrphanRun();
            return;
        }
        if (result == LineDecodeResult::Continue)
        {
            if (pending)
            {
                flushOrphanRun();
                if (!pending->continuationBytes.empty() || !pending->rawText.empty())
                {
                    pending->continuationBytes.push_back('\n');
                }
                pending->continuationBytes.append(trimmed.data(), trimmed.size());
            }
            else
            {
                // Extend the current orphan run instead of emitting
                // a new error per line; `flushOrphanRun` compresses
                // the run to a single "lines N-M" summary the first
                // time a real header (or EOF) arrives.
                if (orphanRunFirstLine == 0)
                {
                    orphanRunFirstLine = lineNumber;
                }
                orphanRunLastLine = lineNumber;
            }
            return;
        }
        // Any future `LineDecodeResult` variant added here without a
        // handler would silently fall through to the Emit path. Assert
        // + `std::unreachable()` in release makes that impossible.
        assert(result == LineDecodeResult::Emit);
        if (result != LineDecodeResult::Emit)
        {
            std::unreachable();
        }

        // Emit: commit any open record, then open a new one from
        // the freshly-decoded fields. Sort happens at commit time
        // (fields may have been re-ordered by continuation splice).
        flushOrphanRun();
        commitPending();

        PendingRecord fresh;
        fresh.rawText.assign(trimmed.data(), trimmed.size());
        fresh.ownedArena = std::move(ownedArena);
        fresh.compactValues = std::move(compactValues);
        fresh.startLineNumber = lineNumber;
        fresh.openedAt = std::chrono::steady_clock::now();
        // Only decoders that expose `LastContinuationTarget()` can
        // route continuation bytes. Others (JSON, CSV) leave it
        // `INVALID_KEY_ID`, which means any subsequent `Continue`
        // would surface an orphan error — but they never return
        // `Continue`, so this branch is inert for them.
        if constexpr (requires { decoder.LastContinuationTarget(); })
        {
            fresh.continuationTarget = decoder.LastContinuationTarget();
        }
        pending = std::move(fresh);

        // Reset the moved-from scratch buffers to a known-empty
        // valid state. `DecodeCompact` also calls `out.clear()` on
        // entry, but the analyzer can't see through the template
        // decoder's body, so this prevents a `clang-analyzer-
        // cplusplus.Move` false positive when the next iteration
        // calls `compactValues.begin()`.
        compactValues.clear();
        ownedArena.clear();
    };

    bool reachedEof = false;
    while (!reachedEof)
    {
        if (stopToken.stop_requested())
        {
            break;
        }

        const size_t read = producer->Read(std::span<char>(readBuffer.data(), readBuffer.size()));
        if (read != 0)
        {
            carry.append(readBuffer.data(), read);
        }
        else
        {
            if (producer->IsClosed())
            {
                reachedEof = true;
            }
            else
            {
                // Age-out: a decoder that opened a record and only
                // fed continuations must not stall forever on a
                // quiet producer. Commit the accumulated form so
                // the GUI can see it, then wait for more bytes.
                if (pending)
                {
                    const auto age = std::chrono::steady_clock::now() - pending->openedAt;
                    if (age >= STREAMING_PENDING_RECORD_MAX_AGE)
                    {
                        commitPending();
                    }
                }
                coalescer.TryFlush(false);
                producer->WaitForBytes(STREAMING_BATCH_FLUSH_INTERVAL);
                continue;
            }
        }

        size_t scanStart = 0;
        while (scanStart < carry.size())
        {
            const size_t newlineRel = carry.find('\n', scanStart);
            if (newlineRel == std::string::npos)
            {
                break;
            }
            std::string_view line(carry.data() + scanStart, newlineRel - scanStart);
            processLine(line);
            scanStart = newlineRel + 1;

            if (stopToken.stop_requested())
            {
                break;
            }
        }
        if (scanStart > 0)
        {
            carry.erase(0, scanStart);
        }

        coalescer.TryFlush(false);
    }

    if (!carry.empty() && !stopToken.stop_requested())
    {
        std::string_view line(carry);
        processLine(line);
        carry.clear();
    }

    // EOF / cancellation: commit whatever pending we still hold so
    // the record isn't lost, and surface any still-open orphan run
    // as its final "lines N-M" summary. `Finish` then emits its
    // terminal batch.
    flushOrphanRun();
    commitPending();

    coalescer.Finish(nextLineNumber, stopToken.stop_requested());
}

} // namespace loglib::internal
