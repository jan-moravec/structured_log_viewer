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
     * @brief Append a single line-boundary offset; @p position must be
     * strictly greater than the previously registered one. Used by
     * tests; the parser uses `AppendLineOffsets` for batched updates.
     */
    void RegisterLineEnd(size_t position);

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
        return !mMultiLineSpans.empty();
    }

    /**
     * @brief Sliding view over the owned-string arena (escape-decoded values
     * that cannot live in the mmap). `LogLine` materialisation indexes
     * into this via `(offset, length)` stored in its compact values.
     */
    std::string_view OwnedStringsView() const noexcept;

    /**
     * @brief Append @p bytes to the owned-string arena and return the byte
     * offset of the first appended byte. Single-threaded contract: the
     * streaming pipeline serialises arena writes through Stage C.
     */
    uint64_t AppendOwnedStrings(std::string_view bytes);

    /** @brief Heap bytes owned by `mOwnedStrings` (capacity). */
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
     * @brief Concatenated escape-decoded strings referenced by this file's
     * `LogLine` values via `(offset, length)`.
     */
    std::string mOwnedStrings;

    /** @brief Maps each multi-line header to its final physical line. */
    std::unordered_map<size_t, size_t> mMultiLineSpans;
};

} // namespace loglib
