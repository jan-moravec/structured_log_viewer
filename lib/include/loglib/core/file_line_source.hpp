/**
 * @brief Line source for a log file of fixed size, mapped into memory or held as bytes.
 *
 * A FileLineSource maps the file and records where each line starts. Rows keep offsets into the mapping, so rows from a
 * static file never copy their text. The source also owns an arena for strings that cannot stay views into the mapping,
 * such as decoded escapes, and the line ranges of multi-line records. It is used when a file is opened as a whole, as
 * opposed to followed live. FromBytes builds the same source over text that is already in memory, which lets tests and
 * callers that hold the text use the file pipeline without a file.
 *
 * The line id of a row is the zero-based physical line of the first line of its record, so it indexes the line offsets
 * of the file. A record that spans several lines has the id of its first line, and the other lines have no row of their
 * own. An id is not a count of lines or records, because blank lines, lines that failed to parse and continuation lines
 * take up physical lines too.
 *
 * Views into the bytes and into the arena stay valid until the source is destroyed, so a source must outlive the rows
 * built from it. A source cannot be moved, because rows point at it.
 *
 * This is part of the core module.
 */
#pragma once

#include "loglib/core/line_source.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace loglib
{

namespace internal
{
class StableByteArena;
}

/**
 * @brief LineSource for a file that has a fixed size.
 *
 * Static parsing runs directly over the bytes, and the rows it produces resolve their MmapSlice and OwnedString cells
 * through this source. It cannot evict lines.
 */
class FileLineSource final : public LineSource
{
public:
    /** @brief Maps the file. Throws IoError if it cannot be opened or mapped. */
    explicit FileLineSource(const std::filesystem::path &filePath);

    /**
     * @brief Maps storagePath but reports logicalPath as the source, for a file that was unpacked to a temporary copy.
     *
     * Throws IoError if the file cannot be opened or mapped.
     */
    FileLineSource(std::filesystem::path storagePath, std::filesystem::path logicalPath);

    /**
     * @brief Serves bytes held in memory as if they were a mapped file.
     *
     * The bytes never change afterwards, so views into them stay valid for the life of the source. An empty string
     * behaves like an empty file. The line offsets start empty, as for a mapped file, and the parsers fill them.
     *
     * @param displayName The path to report for the source.
     */
    [[nodiscard]] static std::unique_ptr<FileLineSource> FromBytes(
        std::string bytes, std::filesystem::path displayName
    );

    /** @brief Unmaps the file, then releases the lifetime anchors in the reverse order of attaching. */
    ~FileLineSource() override;

    FileLineSource(const FileLineSource &) = delete;
    FileLineSource &operator=(const FileLineSource &) = delete;

    FileLineSource(FileLineSource &&) = delete;
    FileLineSource &operator=(FileLineSource &&) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const noexcept override;

    /**
     * @brief The text of a line without its carriage return and line feed.
     *
     * For the first line of a multi-line record the result covers the whole record. A line past the last one is a
     * programming error and asserts.
     */
    [[nodiscard]] std::string RawLine(size_t lineId) const override;

    [[nodiscard]] std::string_view ResolveMmapBytes(
        uint64_t offset, uint32_t length, size_t lineId
    ) const noexcept override;

    /**
     * @brief Returns bytes from the owned-string arena.
     *
     * Safe to call while another thread appends. The view stays valid for the life of the source.
     */
    [[nodiscard]] std::string_view ResolveOwnedBytes(
        uint64_t offset, uint32_t length, size_t lineId
    ) const noexcept override;

    /** @brief The mapped bytes, or the bytes given to FromBytes. Empty for an empty file. */
    [[nodiscard]] std::span<const char> StableBytes() const noexcept override;

    /** @brief Appends bytes to the owned-string arena, which every line shares. Safe from any thread. */
    uint64_t AppendOwnedBytes(size_t lineId, std::string_view bytes) override;

    [[nodiscard]] bool SupportsEviction() const noexcept override;

    void EvictBefore(size_t firstSurvivingLineId) override;
    [[nodiscard]] size_t FirstAvailableLineId() const noexcept override;

    /** @brief The number of physical lines whose offsets are stored. */
    [[nodiscard]] size_t LineCount() const;

    /** @brief Reserves room for count line offsets, to avoid regrowing while a parse appends them. */
    void ReserveLineOffsets(size_t count);

    /** @brief Appends line start offsets. They must be strictly increasing and start after the last stored offset. */
    void AppendLineOffsets(const std::vector<uint64_t> &offsets);

    /**
     * @brief Records that the physical lines headerLineId to lastLineId form one record.
     *
     * Both are zero-based and inclusive, and lastLineId must be greater than headerLineId. A single-line span is
     * ignored. Register the offsets of both lines first.
     */
    void RegisterMultiLineRecord(size_t headerLineId, size_t lastLineId);

    /** @brief True when at least one multi-line record is registered. */
    [[nodiscard]] bool HasMultiLineRecords() const noexcept
    {
        return !mMultiLineSpans.empty();
    }

    /**
     * @brief Adds bytes to the end of an owned string, in place, when that string is the newest data in the arena.
     *
     * Returns false when it is not, and then the caller must append a full copy instead.
     */
    bool TryExtendOwnedString(uint64_t offset, uint64_t length, std::string_view bytes);

    /**
     * @brief Keeps anchor alive until after the file is unmapped, for example the owner of an unpacked temporary file.
     *
     * Anchors are released in the reverse order of attaching.
     */
    void AttachLifetimeAnchor(std::shared_ptr<void> anchor) noexcept;

    /** @brief Heap bytes held by the line offsets, counting capacity. Used by the memory benchmark. */
    [[nodiscard]] size_t LineOffsetsMemoryBytes() const noexcept;

    /** @brief Heap bytes reserved by the owned-string arena. */
    [[nodiscard]] size_t OwnedStringsMemoryBytes() const noexcept;

private:
    struct Backing;
    struct InMemoryTag
    {
    };

    /** @brief Builds a source over memory. Used by FromBytes. The tag keeps it apart from the two-path constructor. */
    FileLineSource(InMemoryTag, std::filesystem::path displayName, std::string bytes);

    /** @brief Registers the first line offset, which every source starts with. */
    void InitializeOffsets();

    std::filesystem::path mPath;
    std::filesystem::path mStoragePath;

    /** @brief Declared before mBacking so that destruction unmaps first. */
    std::shared_ptr<void> mLifetimeAnchor;

    /** @brief The memory mapping or the owned bytes. */
    std::unique_ptr<Backing> mBacking;

    /** @brief Start and size of the bytes in mBacking, cached because every cell lookup reads them. */
    const char *mData = nullptr;
    size_t mSize = 0;

    /** @brief Byte offsets of every line start, plus one past the end of the last line. */
    std::vector<uint64_t> mLineOffsets;

    /** @brief Decoded strings that LogLine values refer to by offset and length. */
    std::unique_ptr<internal::StableByteArena> mOwnedStrings;

    /** @brief Maps the first line of each multi-line record to its last line. */
    std::unordered_map<size_t, size_t> mMultiLineSpans;
};

} // namespace loglib
