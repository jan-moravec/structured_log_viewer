/**
 * @brief Common interface to the bytes behind parsed log rows.
 *
 * A LogLine stores offsets and lengths rather than text, so it needs something that can turn them back into bytes.
 * LineSource is that link. FileLineSource serves bytes from a memory-mapped file and StreamLineSource serves them from
 * lines held in memory, which lets the rest of the library treat a static file and a live stream the same way. The
 * header also holds the retention limits for streamed lines.
 *
 * A row's line id means something different for each source, and the row's source pointer tells which one applies. For
 * a FileLineSource it is the zero-based physical line of the first line of the record. For a StreamLineSource it is a
 * one-based number that AppendLine gives to each record, and a record that spans several lines gets one id. An id is
 * therefore a key for RawLine, not a line count. Do not turn ids into line counts.
 *
 * This is part of the core module.
 */
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

/** @brief Default number of streamed lines kept before EvictBefore drops older lines. */
inline constexpr size_t DEFAULT_RETENTION_LINES = 10'000;

/** @brief Minimum number of streamed lines a retention cap may keep. */
inline constexpr size_t MIN_RETENTION_LINES = 1'000;

/** @brief Maximum number of streamed lines a retention cap may keep. */
inline constexpr size_t MAX_RETENTION_LINES = 1'000'000;

/**
 * @brief Owns the bytes behind log lines and turns offsets and lengths back into text.
 *
 * A LogLine does not hold text. It holds a pointer to its LineSource and offsets, which keeps rows small and lets a
 * memory-mapped file and a live stream look the same. A resolved view stays valid until its line is evicted.
 */
class LineSource
{
public:
    LineSource() = default;
    virtual ~LineSource() = default;

    LineSource(const LineSource &) = delete;
    LineSource &operator=(const LineSource &) = delete;

    LineSource(LineSource &&) = delete;
    LineSource &operator=(LineSource &&) = delete;

    /** @brief Name to show for the source, usually its file path. */
    [[nodiscard]] virtual const std::filesystem::path &Path() const noexcept = 0;

    /**
     * @brief The text of a line without its line ending. Throws std::out_of_range if the line was evicted or does not
     * exist yet.
     */
    [[nodiscard]] virtual std::string RawLine(size_t lineId) const = 0;

    /** @brief Returns the bytes of a MmapSlice cell. Empty for a source whose bytes are not stable. */
    [[nodiscard]] virtual std::string_view ResolveMmapBytes(
        uint64_t offset, uint32_t length, size_t lineId
    ) const noexcept = 0;

    /** @brief Returns the bytes of an OwnedString cell. Empty if the range is not stored. */
    [[nodiscard]] virtual std::string_view ResolveOwnedBytes(
        uint64_t offset, uint32_t length, size_t lineId
    ) const noexcept = 0;

    /**
     * @brief The bytes a parser may point MmapSlice cells into. A file source returns its mapping, a stream source
     * nothing.
     */
    [[nodiscard]] virtual std::span<const char> StableBytes() const noexcept = 0;

    /** @brief True when parsers can use MmapSlice cells with this source. */
    [[nodiscard]] bool BytesAreStable() const noexcept
    {
        return !StableBytes().empty();
    }

    /** @brief Stores decoded bytes for a line and returns the offset to put in its OwnedString cell. */
    virtual uint64_t AppendOwnedBytes(size_t lineId, std::string_view bytes) = 0;

    /** @brief True when EvictBefore removes lines. A file source cannot evict. */
    [[nodiscard]] virtual bool SupportsEviction() const noexcept = 0;

    /** @brief Drops every line with an id below firstSurvivingLineId. No live row may still refer to those lines. */
    virtual void EvictBefore(size_t firstSurvivingLineId) = 0;

    /** @brief The lowest line id that can still be resolved. */
    [[nodiscard]] virtual size_t FirstAvailableLineId() const noexcept = 0;

    /**
     * @brief The registry that turns enumeration cells into text. LogTable sets it, and it must outlive every row that
     * uses it. It is not synchronized, so set it from the thread that owns the table.
     */
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
