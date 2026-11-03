/**
 * @brief Memory-mapped log file with its line offsets.
 *
 * LogFile maps the file into memory and records where each line starts. It also owns the arenas that hold strings which
 * could not stay as views into the file. Rows keep views into the mapping, so a LogFile must outlive the rows built
 * from it. Moving a LogFile keeps the mapped pointer stable.
 *
 * This is part of the core module.
 */
#pragma once

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
 * @brief Owns the mapping of one log file, so string views into its content stay valid.
 *
 * LogFile only holds the bytes, the line offsets and the arenas. FileLineSource does the addressing of single records.
 */
class LogFile
{
public:
    /** @brief Maps the file. Throws std::runtime_error if it cannot be opened or mapped. */
    explicit LogFile(const std::filesystem::path &filePath);

    /**
     * @brief Maps storagePath but reports logicalPath as the source, for files that were unpacked to a temporary copy.
     */
    LogFile(std::filesystem::path storagePath, std::filesystem::path logicalPath);

    /** @brief Unmaps the file, then releases the lifetime anchors. */
    ~LogFile();

    LogFile(const LogFile &) = delete;
    LogFile &operator=(const LogFile &) = delete;

    /** @brief Takes over the mapping without copying bytes, so views into Data stay valid. */
    LogFile(LogFile &&) noexcept;

    /** @brief Deleted, because assignment could release the temporary file before the old mapping is replaced. */
    LogFile &operator=(LogFile &&) noexcept = delete;

    /** @brief The path to report to the user, which can differ from the path that is mapped. */
    const std::filesystem::path &GetPath() const;

    /** @brief Start of the mapped bytes, or null when nothing is mapped. */
    const char *Data() const;

    size_t Size() const;

    /**
     * @brief Returns the text of a line without its trailing carriage return.
     *
     * For the first line of a multi-line record the result covers the whole record. Throws std::out_of_range when
     * lineNumber is past the last line.
     */
    std::string GetLine(size_t lineNumber) const;

    size_t GetLineCount() const;

    /** @brief Reserves room for count line offsets, to avoid regrowing while a parse appends them. */
    void ReserveLineOffsets(size_t count);

    /** @brief Appends line start offsets. They must be strictly increasing and start after the last stored offset. */
    void AppendLineOffsets(const std::vector<uint64_t> &offsets);

    /** @brief Heap bytes held by the line offsets, counting capacity. Used by the memory benchmark. */
    size_t LineOffsetsMemoryBytes() const noexcept;

    /**
     * @brief Records that the physical lines headerLineId to lastLineId form one record.
     *
     * Both are zero-based and inclusive, and lastLineId must be greater than headerLineId. A single-line span is
     * ignored.
     */
    void RegisterMultiLineRecord(size_t headerLineId, size_t lastLineId);

    /** @brief True when at least one multi-line record is registered. */
    [[nodiscard]] bool HasMultiLineRecords() const noexcept
    {
        return mMultiLineSpans && !mMultiLineSpans->empty();
    }

    /**
     * @brief Returns bytes from the owned-string arena, which holds decoded values that cannot stay as views into the
     * mapping.
     *
     * Safe to call while another thread appends. The view stays valid for the lifetime of the file.
     *
     * @param offset Offset returned by AppendOwnedStrings.
     * @param length Number of bytes.
     * @return The bytes, or an empty view when the range was never stored.
     */
    [[nodiscard]] std::string_view OwnedString(uint64_t offset, uint64_t length) const noexcept;

    /** @brief Appends bytes to the owned-string arena and returns the offset of the first one. Safe from any thread. */
    uint64_t AppendOwnedStrings(std::string_view bytes);

    /**
     * @brief Adds bytes to the end of an owned string, in place, when that string is the newest data in the arena.
     *
     * Returns false when it is not, and then the caller must append a full copy instead.
     */
    bool TryExtendOwnedString(uint64_t offset, uint64_t length, std::string_view bytes);

    /** @brief Heap bytes reserved by the owned-string arena. */
    size_t OwnedStringsMemoryBytes() const noexcept;

    /**
     * @brief Keeps anchor alive until after the file is unmapped, for example the owner of an unpacked temporary file.
     *
     * Anchors are released in the reverse order of attaching.
     */
    void AttachLifetimeAnchor(std::shared_ptr<void> anchor) noexcept;

private:
    struct Mapping;

    std::filesystem::path mPath;
    std::filesystem::path mStoragePath;

    /** @brief Declared before mMapping so that destruction unmaps first. */
    std::shared_ptr<void> mLifetimeAnchor;

    std::unique_ptr<Mapping> mMapping;

    /** @brief Byte offsets of every line start, plus one past the end of the last line. */
    std::vector<uint64_t> mLineOffsets;

    /** @brief Decoded strings that LogLine values refer to by offset and length. */
    std::unique_ptr<internal::StableByteArena> mOwnedStrings;

    /**
     * @brief Maps the first line of each multi-line record to its last line. Held by pointer to keep the move cheap.
     */
    std::unique_ptr<std::unordered_map<size_t, size_t>> mMultiLineSpans{
        std::make_unique<std::unordered_map<size_t, size_t>>(),
    };
};

} // namespace loglib
