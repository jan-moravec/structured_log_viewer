#pragma once

#include "loglib/key_index.hpp"
#include "loglib/log_value.hpp"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace loglib
{

/// Reference to a single source line emitted by a streaming `LogSource`
/// (`TailingFileSource`, future `StdinSource` / `TcpSource` / ...).
///
/// Mirrors the public surface of `LogFileReference` so that the
/// `LogModelItemDataRole::CopyLine` path in the GUI can branch on the
/// row's variant and call `GetLine()` on either type uniformly. Unlike
/// `LogFileReference`, `StreamLineReference` does **not** point into a
/// memory-mapped buffer — `TailingFileSource` is forbidden from mmap'ing
/// its target (PRD §7 *No mmap on the tail*) — so it owns its raw bytes
/// directly (PRD 4.9.7.ii, OQ-9 first-cut). The trade-off vs sharing
/// reference-counted byte chunks is documented in the PRD's OQ-9 entry;
/// per-line ownership is the simpler choice and is revisited only if a
/// soak test shows the per-line raw-bytes term dominates the resident
/// set.
class StreamLineReference
{
public:
    /// @param sourceName   `LogSource::DisplayName()` of the producing source —
    ///                     typically the file path's string form for
    ///                     `TailingFileSource` (PRD 4.10.4).
    /// @param rawLine      Original raw line bytes (without the trailing
    ///                     `\n`). Owned by the reference for its lifetime.
    /// @param lineId       Session-local monotonic line id (PRD 4.10.4):
    ///                     starts at 1 at `BeginStreaming` time, increments
    ///                     across rotations. Tests, the streaming line
    ///                     ledger, and error-line composition use it as
    ///                     the canonical identity.
    StreamLineReference(std::string sourceName, std::string rawLine, size_t lineId);

    StreamLineReference(const StreamLineReference &) = default;
    StreamLineReference &operator=(const StreamLineReference &) = default;
    StreamLineReference(StreamLineReference &&) noexcept = default;
    StreamLineReference &operator=(StreamLineReference &&) noexcept = default;

    /// Returns the source's display name as a `std::filesystem::path` so
    /// callers can drop the reference into APIs that already take a
    /// `LogFileReference::GetPath()`-compatible value.
    [[nodiscard]] const std::filesystem::path &GetPath() const noexcept;

    /// Session-local monotonic `LineId` (1-based). Mirrors
    /// `LogFileReference::GetLineNumber()`.
    [[nodiscard]] size_t GetLineNumber() const noexcept;

    /// Returns the owned raw line bytes. Mirrors
    /// `LogFileReference::GetLine()`; unlike that overload, no `\r`-trimming
    /// or mmap dereference happens here — the bytes are exactly what the
    /// source emitted (without the trailing newline).
    [[nodiscard]] const std::string &GetLine() const noexcept;

    /// Internal: bumps the stored `LineId` by @p delta. Reserved for any
    /// future stream-side line-renumbering needs (mirrors
    /// `LogFileReference::ShiftLineNumber`); not currently used.
    void ShiftLineNumber(size_t delta) noexcept;

private:
    std::filesystem::path mPath;
    std::string mRawLine;
    size_t mLineNumber = 0;
};

/// Owning sibling of `LogLine` for streaming sources.
///
/// Where `LogLine` stores its values as 16-byte compact tagged unions that
/// reference a parent `LogFile`'s mmap (`MmapSlice`) or its owned-string
/// arena (`OwnedString`), `StreamLogLine` owns the parsed values directly:
/// `std::vector<std::pair<KeyId, LogValue>>` with `std::string` payloads,
/// no arena, no `MmapSlice`, no `LogFile *` (PRD 4.9.7.i). The tail source
/// has no mmap to alias into, and survives file rotation (the on-disk
/// bytes may be gone before the row is evicted) — owning the values
/// removes a whole class of dangling-view bugs at the cost of a few
/// `std::string` allocations per line.
///
/// `LogTable` ingests both `LogLine` and `StreamLogLine`; the per-row
/// dispatch decision (variant per row vs separate vectors) is documented
/// in `log_table.hpp` (PRD 4.9.7).
class StreamLogLine
{
public:
    /// Cold-path ctor: takes pre-built sorted `(KeyId, LogValue)` pairs.
    /// `sortedValues` must be ascending on `pair::first`. `string_view`
    /// payloads are copied into owned `std::string`s so the line is
    /// self-contained.
    StreamLogLine(
        std::vector<std::pair<KeyId, LogValue>> sortedValues, const KeyIndex &keys, StreamLineReference fileReference
    );

    /// Map-form convenience ctor used by tests and any ingest path that
    /// does not produce sorted pairs already.
    StreamLogLine(const LogMap &values, KeyIndex &keys, StreamLineReference fileReference);

    StreamLogLine(const StreamLogLine &) = delete;
    StreamLogLine &operator=(const StreamLogLine &) = delete;

    StreamLogLine(StreamLogLine &&) noexcept = default;
    StreamLogLine &operator=(StreamLogLine &&) noexcept = default;

    /// Returns `std::monostate` if @p id is not present on this line.
    [[nodiscard]] LogValue GetValue(KeyId id) const;
    [[nodiscard]] LogValue GetValue(const std::string &key) const;

    /// Inserts or replaces the value at @p id. Owned-payload semantics:
    /// `string_view` inputs are copied into a new `std::string`; the
    /// `LogValueTrustView` overload from `LogLine` does not exist here
    /// because there is no stable mmap to alias into.
    void SetValue(KeyId id, LogValue value);
    void SetValue(const std::string &key, LogValue value);

    [[nodiscard]] std::vector<std::string> GetKeys() const;

    /// `(KeyId, LogValue)` pairs in ascending `KeyId` order. Returned by
    /// const-reference because the storage is already in the public
    /// `LogValue` shape — no `Materialise` step is needed.
    [[nodiscard]] const std::vector<std::pair<KeyId, LogValue>> &IndexedValues() const noexcept;

    [[nodiscard]] LogMap Values() const;

    /// Used when migrating a line into a different `LogData`'s canonical
    /// `KeyIndex` (mirrors `LogLine::RebindKeys`). The KeyIds themselves
    /// are *not* remapped here; callers that re-key must construct a new
    /// `StreamLogLine` with remapped pairs.
    void RebindKeys(const KeyIndex &keys) noexcept;

    [[nodiscard]] const KeyIndex &Keys() const noexcept;

    [[nodiscard]] const StreamLineReference &FileReference() const noexcept;
    [[nodiscard]] StreamLineReference &FileReference() noexcept;

    /// Number of stored fields. Mirrors `LogLine::ValueCount`.
    [[nodiscard]] size_t ValueCount() const noexcept;

    /// Sum of owned heap bytes attributable to this line (capacity of
    /// the values vector plus payload bytes of each owned string).
    /// Used by the memory-footprint benchmark; not part of the parse hot
    /// path.
    [[nodiscard]] size_t OwnedMemoryBytes() const noexcept;

private:
    /// Linear scan over the sorted values (typical line is small;
    /// matches `LogLine::FindCompact` strategy).
    [[nodiscard]] const LogValue *FindValue(KeyId id) const noexcept;
    [[nodiscard]] LogValue *FindValueMutable(KeyId id) noexcept;

    std::vector<std::pair<KeyId, LogValue>> mValues;
    const KeyIndex *mKeys = nullptr;
    StreamLineReference mFileReference;
};

} // namespace loglib
