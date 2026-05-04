#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace loglib
{

/// Abstract source of logical log lines. Owns the bytes backing each
/// line (raw text and escape-decoded payloads referenced by `LogLine`
/// compact values) and resolves byte ranges by offset/length so
/// `LogLine::GetValue` can dispatch through this seam regardless of
/// whether the storage is a memory-mapped file (`FileLineSource`) or
/// per-line owned buffers (`StreamLineSource`).
///
/// `lineId` semantics are source-defined: `FileLineSource` uses 0-based
/// file-line indices, `StreamLineSource` uses 1-based monotonic ids
/// assigned by `AppendLine`. A `LogLine`'s `mSource` pointer
/// disambiguates which id space applies.
///
/// Resolution methods return views valid until the line is evicted;
/// sources without eviction support hold them for their lifetime.
class LineSource
{
public:
    LineSource() = default;
    virtual ~LineSource() = default;

    LineSource(const LineSource &) = delete;
    LineSource &operator=(const LineSource &) = delete;

    LineSource(LineSource &&) = delete;
    LineSource &operator=(LineSource &&) = delete;

    /// Display name; typically a filesystem path.
    [[nodiscard]] virtual const std::filesystem::path &Path() const noexcept = 0;

    /// Raw line text for @p lineId, trailing CR/LF stripped. Cold path
    /// (used by `Copy raw line`). Throws `std::out_of_range` if
    /// @p lineId has been evicted or not yet produced.
    [[nodiscard]] virtual std::string RawLine(size_t lineId) const = 0;

    /// Resolve a `CompactTag::MmapSlice` payload. Returns an empty view
    /// when `BytesAreStable() == false`. `lineId` is unused by mmap
    /// sources but part of the interface for per-line-arena sources.
    [[nodiscard]] virtual std::string_view
    ResolveMmapBytes(uint64_t offset, uint32_t length, size_t lineId) const noexcept = 0;

    /// Resolve a `CompactTag::OwnedString` payload. View is valid
    /// until @p lineId is evicted. Out-of-range returns an empty view.
    [[nodiscard]] virtual std::string_view
    ResolveOwnedBytes(uint64_t offset, uint32_t length, size_t lineId) const noexcept = 0;

    /// Bytes a parser may safely emit `MmapSlice` payloads against.
    /// `FileLineSource` returns the file mmap; `StreamLineSource`
    /// returns an empty span (everything must go through `OwnedString`).
    [[nodiscard]] virtual std::span<const char> StableBytes() const noexcept = 0;

    /// True iff this source can serve `MmapSlice` resolutions.
    [[nodiscard]] bool BytesAreStable() const noexcept
    {
        return !StableBytes().empty();
    }

    /// Append @p bytes to the owned-bytes arena for @p lineId; returns
    /// the offset that a `CompactTag::OwnedString` payload should
    /// reference. Cold path used by `LogLine::SetValue` to promote a
    /// `LogValue{string}` into stable storage. The parser hot path uses
    /// its own bulk append, not this method.
    virtual uint64_t AppendOwnedBytes(size_t lineId, std::string_view bytes) = 0;

    /// True iff `EvictBefore` actually evicts; otherwise a no-op so
    /// generic plumbing (`LogTable::EvictPrefixRows`) need not branch.
    [[nodiscard]] virtual bool SupportsEviction() const noexcept = 0;

    /// Drop every line with id < @p firstSurvivingLineId. Caller must
    /// ensure no live `LogLine` references the evicted ids.
    /// No-op when `SupportsEviction() == false`.
    virtual void EvictBefore(size_t firstSurvivingLineId) = 0;

    /// First lineId still resolvable. Advances by `EvictBefore`.
    [[nodiscard]] virtual size_t FirstAvailableLineId() const noexcept = 0;
};

} // namespace loglib
