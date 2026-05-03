#pragma once

#include "loglib/internal/compact_log_value.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_value.hpp"

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace loglib
{

class LineSource;

/// One log record, stored as a sorted-by-`KeyId` flat vector of
/// `(KeyId, internal::CompactLogValue)` pairs. Each compact value is 16 B
/// (vs the public `LogValue` variant's ~48 B); strings are
/// `(offset, length)` pairs resolved through the owning `LineSource`
/// (file mmap or per-line stream arena).
///
/// The `LineSource * + size_t lineId` pair on every `LogLine` is the
/// session-wide row-identity primitive: it addresses the line within
/// the source's id space (file lines: 0-based; stream lines: 1-based
/// monotonic from `StreamLineSource::AppendLine`) and gives every
/// `LogValue` materialisation call a single dispatch point through the
/// source's virtuals -- no static-vs-streaming bifurcation in the
/// `LogLine` ABI.
class LogLine
{
public:
    /// Cold-path ctor: convert a public-variant value list into compact
    /// storage. Owned strings are appended to the source's arena
    /// (file: shared `LogFile::mOwnedStrings`; stream: per-line arena
    /// for @p lineId); `string_view` values pointing inside the
    /// source's `StableBytes()` range are stored zero-copy as
    /// `MmapSlice`.
    LogLine(
        std::vector<std::pair<KeyId, LogValue>> sortedValues,
        const KeyIndex &keys,
        LineSource &source,
        size_t lineId
    );

    /// Hot-path ctor: takes pre-built compact values (e.g. from the
    /// streaming JSON parser). `sortedValues` must be ascending on
    /// `pair::first`. `OwnedString` payloads must already be relative
    /// to the arena that ultimately owns them.
    LogLine(
        std::vector<std::pair<KeyId, internal::CompactLogValue>> sortedValues,
        const KeyIndex &keys,
        LineSource &source,
        size_t lineId
    );

    /// Cold-path convenience ctor.
    LogLine(const LogMap &values, KeyIndex &keys, LineSource &source, size_t lineId);

    LogLine(const LogLine &) = delete;
    LogLine &operator=(const LogLine &) = delete;

    LogLine(LogLine &&) = default;
    LogLine &operator=(LogLine &&) = default;

    /// Returns `std::monostate` if @p id is not present on this line.
    LogValue GetValue(KeyId id) const;
    LogValue GetValue(const std::string &key) const;

    /// Debug builds assert that @p value is not a `string_view`. Owned
    /// strings written via this overload are appended to the source's
    /// arena (the source mutation is single-threaded).
    void SetValue(KeyId id, LogValue value);

    /// Caller promises any view in @p value outlives the `LogLine`.
    /// Views pointing inside the source's `StableBytes()` range are
    /// stored zero-copy; views outside it are copied into the arena.
    void SetValue(KeyId id, LogValue value, LogValueTrustView trust);

    /// Throws if @p key is unknown.
    void SetValue(const std::string &key, LogValue value);

    std::vector<std::string> GetKeys() const;

    /// (KeyId, LogValue) pairs in ascending KeyId order. Materialises every
    /// compact value into the public variant; allocates a fresh vector —
    /// cold path (used by tests, `LogData::Merge`, and JSON serialisation).
    std::vector<std::pair<KeyId, LogValue>> IndexedValues() const;

    /// Internal: span over the compact storage. Used by hot paths inside
    /// `loglib` (parser pipeline, `LogData::Merge`) that want to walk
    /// fields without materialising a `LogValue` per pair.
    std::span<const std::pair<KeyId, internal::CompactLogValue>> CompactValues() const noexcept;

    LogMap Values() const;

    /// Used by `LogData::Merge` and `LogData` move-ops.
    void RebindKeys(const KeyIndex &keys);

    const KeyIndex &Keys() const;

    /// The `LineSource` this line resolves through. Never null after
    /// construction — every ctor takes a `LineSource&`.
    [[nodiscard]] LineSource *Source() const noexcept;

    /// Line id within the source's id space. File sources: 0-based
    /// `LogFile::GetLine(lineId)` index. Stream sources: 1-based
    /// monotonic id assigned by `StreamLineSource::AppendLine`.
    [[nodiscard]] size_t LineId() const noexcept;

    /// Mutator for the parser's TBB pipeline: Stage C shifts every
    /// line's id by the running line cursor so per-batch relative ids
    /// become absolute.
    void ShiftLineId(size_t delta) noexcept;

    /// Mutator for the parser's per-batch decode stage when it needs
    /// to overwrite a placeholder id (e.g. relative line index assigned
    /// inline). Cold path.
    void SetLineId(size_t lineId) noexcept;

    /// Sum of owned heap bytes attributable to this line: capacity of
    /// `mValues`. Owned-string bytes live in the source's arena and
    /// are accounted for there. Used by the memory-footprint benchmark;
    /// not part of the parse hot path.
    size_t OwnedMemoryBytes() const;

    /// Internal: number of compact values stored.
    size_t ValueCount() const noexcept;

    /// Internal: add @p delta to every `OwnedString` payload. Used by
    /// the parser Stage C and `BufferingSink::OnBatch` when concatenating
    /// a per-batch owned-string buffer onto the canonical source
    /// arena. No-op when @p delta is zero.
    void RebaseOwnedStringOffsets(uint64_t delta) noexcept;

    /// Internal: returns true when @p id is stored as an `MmapSlice`
    /// (zero-copy fast path). Used by the `[allocations]` benchmark.
    bool IsMmapSlice(KeyId id) const noexcept;

    /// Internal: returns true when @p id is stored as an `OwnedString`
    /// (escape-decoded slow path).
    bool IsOwnedString(KeyId id) const noexcept;

private:
    /// Hot path: linear scan over the small (typically <=8 entry) sorted
    /// span via `lower_bound`. Returns `nullptr` if @p id is absent.
    const internal::CompactLogValue *FindCompact(KeyId id) const noexcept;

    internal::CompactLineFields mValues;
    const KeyIndex *mKeys = nullptr;
    LineSource *mSource = nullptr;
    size_t mLineId = 0;
};

} // namespace loglib
