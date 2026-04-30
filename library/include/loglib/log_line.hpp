#pragma once

#include "loglib/internal/compact_log_value.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_value.hpp"

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace loglib
{

/// One log record, stored as a sorted-by-`KeyId` flat vector of
/// `(KeyId, detail::CompactLogValue)` pairs. Each compact value is 16 B
/// (vs the public `LogValue` variant's ~48 B); strings live either in
/// the parent `LogFile`'s mmap (`MmapSlice`) or in its owned-string
/// arena (`OwnedString`). The public `LogValue` variant is materialised
/// only on access via `GetValue` / `IndexedValues`.
class LogLine
{
public:
    /// Cold-path ctor: convert a public-variant value list into compact
    /// storage. Owned strings are appended to the parent `LogFile`'s arena;
    /// `string_view` values pointing inside the mmap are stored zero-copy.
    LogLine(std::vector<std::pair<KeyId, LogValue>> sortedValues, const KeyIndex &keys, LogFileReference fileReference);

    /// Hot-path ctor: takes pre-built compact values (e.g. from the
    /// streaming JSON parser). `sortedValues` must be ascending on
    /// `pair::first`. `OwnedString` payloads must already be relative
    /// to the arena that ultimately owns them.
    LogLine(
        std::vector<std::pair<KeyId, detail::CompactLogValue>> sortedValues,
        const KeyIndex &keys,
        LogFileReference fileReference
    );

    /// Cold-path convenience ctor.
    LogLine(const LogMap &values, KeyIndex &keys, LogFileReference fileReference);

    LogLine(const LogLine &) = delete;
    LogLine &operator=(const LogLine &) = delete;

    LogLine(LogLine &&) = default;
    LogLine &operator=(LogLine &&) = default;

    /// Returns `std::monostate` if @p id is not present on this line.
    LogValue GetValue(KeyId id) const;
    LogValue GetValue(const std::string &key) const;

    /// Debug builds assert that @p value is not a `string_view`. Owned
    /// strings written via this overload are appended to the parent
    /// `LogFile`'s arena; the file mutation is single-threaded.
    void SetValue(KeyId id, LogValue value);

    /// Caller promises any view in @p value outlives the `LogLine`.
    /// Views pointing inside the parent `LogFile`'s mmap are stored
    /// zero-copy; views outside it are copied into the arena.
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
    std::span<const std::pair<KeyId, detail::CompactLogValue>> CompactValues() const noexcept;

    LogMap Values() const;

    /// Used by `LogData::Merge`.
    void RebindKeys(const KeyIndex &keys);

    const KeyIndex &Keys() const;

    const LogFileReference &FileReference() const;
    LogFileReference &FileReference();

    /// Sum of owned heap bytes attributable to this line: capacity of
    /// `mValues`. Owned-string bytes live in the parent `LogFile` and
    /// are accounted for by `LogFile::OwnedStringsMemoryBytes()`. Used by
    /// the memory-footprint benchmark; not part of the parse hot path.
    size_t OwnedMemoryBytes() const;

    /// Internal: number of compact values stored.
    size_t ValueCount() const noexcept;

    /// Internal: add @p delta to every `OwnedString` payload. Used by
    /// the parser Stage C and `BufferingSink::OnBatch` when concatenating
    /// a per-batch owned-string buffer onto the canonical `LogFile`
    /// arena. No-op when @p delta is zero.
    void RebaseOwnedStringOffsets(uint64_t delta) noexcept;

    /// Internal: returns true when @p id is stored as an `MmapSlice`
    /// (zero-copy fast path). Replaces today's
    /// `holds_alternative<string_view>` check used by the `[allocations]`
    /// benchmark.
    bool IsMmapSlice(KeyId id) const noexcept;

    /// Internal: returns true when @p id is stored as an `OwnedString`
    /// (escape-decoded slow path).
    bool IsOwnedString(KeyId id) const noexcept;

private:
    /// Hot path: linear scan over the small (typically <=8 entry) sorted
    /// span via `lower_bound`. Returns `nullptr` if @p id is absent.
    const detail::CompactLogValue *FindCompact(KeyId id) const noexcept;

    detail::CompactLineFields mValues;
    const KeyIndex *mKeys = nullptr;
    LogFileReference mFileReference;
};

} // namespace loglib
