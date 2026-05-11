#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace loglib
{

class EnumDictionaryRegistry;

/// Abstract source of log lines. Owns the bytes and resolves byte ranges
/// by offset/length so `LogLine` is storage-agnostic (mmap file or
/// per-line owned buffers). `lineId` semantics are source-defined; the
/// `LogLine::mSource` pointer disambiguates id spaces. Resolved views
/// stay valid until the line is evicted.
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

    /// Raw line text for @p lineId, trailing CR/LF stripped. Throws
    /// `std::out_of_range` if @p lineId is evicted or not yet produced.
    [[nodiscard]] virtual std::string RawLine(size_t lineId) const = 0;

    /// Resolve a `CompactTag::MmapSlice` payload; empty if unstable.
    [[nodiscard]] virtual std::string_view ResolveMmapBytes(uint64_t offset, uint32_t length, size_t lineId)
        const noexcept = 0;

    /// Resolve a `CompactTag::OwnedString` payload; empty if out of range.
    [[nodiscard]] virtual std::string_view ResolveOwnedBytes(uint64_t offset, uint32_t length, size_t lineId)
        const noexcept = 0;

    /// Bytes a parser may emit `MmapSlice` payloads against.
    /// File sources return the mmap; stream sources return empty.
    [[nodiscard]] virtual std::span<const char> StableBytes() const noexcept = 0;

    /// True iff this source can serve `MmapSlice` resolutions.
    [[nodiscard]] bool BytesAreStable() const noexcept
    {
        return !StableBytes().empty();
    }

    /// Append @p bytes for @p lineId; returns the offset for an
    /// `OwnedString` payload.
    virtual uint64_t AppendOwnedBytes(size_t lineId, std::string_view bytes) = 0;

    /// True iff `EvictBefore` actually evicts.
    [[nodiscard]] virtual bool SupportsEviction() const noexcept = 0;

    /// Drop every line with id < @p firstSurvivingLineId. Caller ensures
    /// no live `LogLine` references the evicted ids.
    virtual void EvictBefore(size_t firstSurvivingLineId) = 0;

    /// First lineId still resolvable.
    [[nodiscard]] virtual size_t FirstAvailableLineId() const noexcept = 0;

    /// Session-wide enum dictionary registry. Set by `LogTable`; must
    /// outlive any referencing `LogLine`. Single-writer: not internally
    /// synchronised.
    [[nodiscard]] const EnumDictionaryRegistry *EnumDictionaries() const noexcept
    {
        return mEnumDictionaries;
    }

    void SetEnumDictionaries(const EnumDictionaryRegistry *registry) noexcept
    {
        mEnumDictionaries = registry;
    }

private:
    const EnumDictionaryRegistry *mEnumDictionaries = nullptr;
};

} // namespace loglib
