#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace loglib
{

/// Abstract source of logical "lines" — i.e. log records as they appear in
/// the viewer. Owns the bytes that back each line (raw text and any
/// escape-decoded payloads referenced by a `LogLine`'s compact values) and
/// exposes byte resolution by offset/length so the polymorphic
/// `LogLine::GetValue` path can dispatch through this seam without knowing
/// whether the underlying storage is a memory-mapped file
/// (`FileLineSource`) or a per-line owned buffer
/// (`StreamLineSource`).
///
/// Conventions:
///
///   - `lineId` semantics are source-defined. `FileLineSource` uses
///     0-based file-line indices (matching `LogFile::GetLine`);
///     `StreamLineSource` uses 1-based monotonic ids assigned by
///     `AppendLine`. The id-space is local to the source: a `LogLine`'s
///     `mSource` pointer disambiguates which space the id belongs to.
///
///   - `OwnedString` payloads on a `LogLine`'s compact values index into
///     whatever arena the source uses for that line. The arena may be
///     session-global (`FileLineSource` shares one arena for the whole
///     mmap) or per-line (`StreamLineSource` keeps a dedicated buffer
///     per row). The interface hides this distinction.
///
///   - All resolution methods return views that are valid until the
///     line is evicted. Sources that do not support eviction
///     (`SupportsEviction() == false`, e.g. `FileLineSource`) hold the
///     view valid for the source's lifetime.
class LineSource
{
public:
    LineSource() = default;
    virtual ~LineSource() = default;

    LineSource(const LineSource &) = delete;
    LineSource &operator=(const LineSource &) = delete;

    LineSource(LineSource &&) = delete;
    LineSource &operator=(LineSource &&) = delete;

    /// Display name. Typically a real filesystem path; non-filesystem
    /// sources (future stdin / TCP / ...) construct a synthetic
    /// `path` from their display string.
    [[nodiscard]] virtual const std::filesystem::path &Path() const noexcept = 0;

    /// Raw line text for @p lineId, with any trailing `\r` / `\n`
    /// stripped. Returned by-value because the file-backed implementation
    /// may need to copy out of the mmap to drop a `\r`. Cold path —
    /// driven by the GUI's `Copy raw line` action.
    ///
    /// Throws `std::out_of_range` if @p lineId has been evicted or has
    /// not yet been produced by the source.
    [[nodiscard]] virtual std::string RawLine(size_t lineId) const = 0;

    /// Resolve a `CompactTag::MmapSlice` payload into the matching byte
    /// view. Only valid on sources where `BytesAreStable() == true`;
    /// other sources return an empty view so a defensive caller (or a
    /// stale compact value) cannot read uninitialised memory.
    ///
    /// `lineId` is unused by the file-backed implementation (the offset
    /// is into a session-global mmap) but is part of the interface so
    /// per-line-arena sources can locate the right buffer.
    [[nodiscard]] virtual std::string_view
    ResolveMmapBytes(uint64_t offset, uint32_t length, size_t lineId) const noexcept = 0;

    /// Resolve a `CompactTag::OwnedString` payload into the matching
    /// byte view. The view is valid until @p lineId is evicted.
    /// Out-of-range `(offset, length)` returns an empty view.
    [[nodiscard]] virtual std::string_view
    ResolveOwnedBytes(uint64_t offset, uint32_t length, size_t lineId) const noexcept = 0;

    /// Stable byte range that this source pins for its lifetime — i.e.
    /// the bytes a parser may safely emit `MmapSlice` compact values
    /// into. `FileLineSource` returns the file's mmap span;
    /// `StreamLineSource` returns an empty span (no stable bytes,
    /// every string-shaped value must land in the `OwnedString` arena
    /// path).
    [[nodiscard]] virtual std::span<const char> StableBytes() const noexcept = 0;

    /// Convenience predicate: true iff `StableBytes()` is non-empty.
    /// Equivalent to "this source can serve `MmapSlice` resolutions".
    [[nodiscard]] bool BytesAreStable() const noexcept
    {
        return !StableBytes().empty();
    }

    /// Append @p bytes to the owned-bytes arena that backs @p lineId
    /// and return the byte offset of the first appended byte. The
    /// returned offset is the value a parser / `LogLine` would stamp
    /// into a `CompactTag::OwnedString` payload; a later
    /// `ResolveOwnedBytes(offset, bytes.size(), lineId)` call returns
    /// the appended view.
    ///
    /// `FileLineSource` ignores @p lineId and forwards into the
    /// session-global `LogFile::mOwnedStrings` arena.
    /// `StreamLineSource` indexes per-line: the call appends to the
    /// arena entry registered by `AppendLine` for @p lineId.
    ///
    /// Used by the cold path in `LogLine::SetValue` when an
    /// `LogValue{string}` / `LogValue{string_view}` payload has to be
    /// promoted into the source's stable storage. The parser hot path
    /// builds `OwnedString` payloads against per-batch / per-line
    /// scratch arenas and rebases or appends them in bulk; this method
    /// is **not** the parser's append seam.
    virtual uint64_t AppendOwnedBytes(size_t lineId, std::string_view bytes) = 0;

    /// True iff this source supports prefix eviction via
    /// `EvictBefore`. False sources accept the call as a no-op so that
    /// generic plumbing (`LogTable::EvictPrefixRows`) need not branch
    /// on type.
    [[nodiscard]] virtual bool SupportsEviction() const noexcept = 0;

    /// Drop every line whose id is strictly less than
    /// @p firstSurvivingLineId. After the call, resolving compact
    /// values that reference an evicted id is undefined; the caller
    /// (typically `LogTable::EvictPrefixRows`) is responsible for
    /// ensuring no live `LogLine` references those ids.
    ///
    /// No-op on sources where `SupportsEviction() == false`.
    virtual void EvictBefore(size_t firstSurvivingLineId) = 0;

    /// First lineId still resolvable through this source. Equals 0 (or
    /// 1, depending on the source's id convention) at construction; the
    /// value advances by every successful `EvictBefore` call. Useful
    /// for assertions and for surfacing eviction state to the GUI.
    [[nodiscard]] virtual size_t FirstAvailableLineId() const noexcept = 0;
};

} // namespace loglib
