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
 * @brief Memory-mapped log file. Owns the mapping so `LogValue` instances can
 * hold `string_view`s into the content; move keeps the pointer stable.
 * Per-record addressing is `FileLineSource`'s job; `LogFile` only
 * holds the bytes and arenas.
 */
class LogFile
{
public:
    /** @brief Throws `std::runtime_error` if the file cannot be opened or mapped. */
    explicit LogFile(const std::filesystem::path &filePath);
    /** @brief Map @p storagePath while reporting @p logicalPath as the source. */
    LogFile(std::filesystem::path storagePath, std::filesystem::path logicalPath);

    /**
     * @brief Out-of-line so the mapping backend can be incomplete. Unmaps
     * before releasing `mLifetimeAnchor`.
     */
    ~LogFile();

    LogFile(const LogFile &) = delete;
    LogFile &operator=(const LogFile &) = delete;

    /**
     * @brief Transfers the mapping without copying bytes. Views into `Data()`
     * remain valid.
     */
    LogFile(LogFile &&) noexcept;
    /**
     * @brief Deleted because member-wise assignment could release the temp
     * file owner before replacing the active mapping.
     */
    LogFile &operator=(LogFile &&) noexcept = delete;

    const std::filesystem::path &GetPath() const;
    const char *Data() const;
    size_t Size() const;

    /** @brief Trailing `'\r'` is trimmed. Throws `std::out_of_range` when out of range. */
    std::string GetLine(size_t lineNumber) const;
    size_t GetLineCount() const;

    void ReserveLineOffsets(size_t count);

    /**
     * @brief Caller must ensure offsets are strictly increasing and start past the
     * current last offset.
     */
    void AppendLineOffsets(const std::vector<uint64_t> &offsets);

    /**
     * @brief Heap bytes owned by `mLineOffsets` (capacity, not size). Used by the
     * memory-footprint benchmark; not part of the parse hot path.
     */
    size_t LineOffsetsMemoryBytes() const noexcept;

    /**
     * @brief Register the inclusive physical-line span for a multi-line
     * record. Indices are zero-based; `lastLineId` must exceed
     * `headerLineId`. Single-line spans are ignored.
     */
    void RegisterMultiLineRecord(size_t headerLineId, size_t lastLineId);

    /** @brief True when at least one multi-line record is registered. */
    [[nodiscard]] bool HasMultiLineRecords() const noexcept
    {
        return mMultiLineSpans && !mMultiLineSpans->empty();
    }

    /**
     * @brief Resolves a range of the owned-string arena (escape-decoded values
     * that cannot live in the mmap).
     *
     * Safe to call while another thread appends. The view stays valid for
     * the lifetime of this file.
     *
     * @param offset Offset returned by `AppendOwnedStrings`.
     * @param length Number of bytes.
     * @return The bytes, or an empty view when the range was never stored.
     */
    [[nodiscard]] std::string_view OwnedString(uint64_t offset, uint64_t length) const noexcept;

    /**
     * @brief Appends @p bytes to the owned-string arena. Safe from any thread.
     * @return The offset of the first appended byte.
     */
    uint64_t AppendOwnedStrings(std::string_view bytes);

    /**
     * @brief Grows the owned string at (@p offset, @p length) in place when it
     * is the arena's most recent data.
     * @return Whether @p bytes were added. Otherwise the caller appends a copy.
     */
    bool TryExtendOwnedString(uint64_t offset, uint64_t length, std::string_view bytes);

    /** @brief Heap bytes reserved by the owned-string arena. */
    size_t OwnedStringsMemoryBytes() const noexcept;

    /**
     * @brief Keep @p anchor alive until after the mmap is released.
     * Multiple anchors are composed in LIFO order.
     */
    void AttachLifetimeAnchor(std::shared_ptr<void> anchor) noexcept;

private:
    struct Mapping;

    std::filesystem::path mPath;
    std::filesystem::path mStoragePath;

    /** @brief Declared before `mMapping` so reverse destruction unmaps first. */
    std::shared_ptr<void> mLifetimeAnchor;

    std::unique_ptr<Mapping> mMapping;

    /** @brief Byte offsets of every line boundary plus a one-past-the-last sentinel. */
    std::vector<uint64_t> mLineOffsets;

    /**
     * @brief Escape-decoded strings referenced by this file's `LogLine` values
     * via `(offset, length)`.
     */
    std::unique_ptr<internal::StableByteArena> mOwnedStrings;

    /**
     * @brief Maps each multi-line header to its final physical line.
     * Held by `unique_ptr` so `LogFile`'s defaulted move is noexcept;
     * `unordered_map`'s move constructor can throw.
     */
    std::unique_ptr<std::unordered_map<size_t, size_t>> mMultiLineSpans{
        std::make_unique<std::unordered_map<size_t, size_t>>()
    };
};

} // namespace loglib
