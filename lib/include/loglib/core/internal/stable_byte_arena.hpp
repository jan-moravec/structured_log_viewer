#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

namespace loglib::internal
{

/**
 * @brief Append-only byte store whose bytes never move.
 *
 * Writers serialise on an internal mutex. `View` takes no lock and may run
 * concurrently with writers: a returned view stays valid for the arena's
 * lifetime. Offsets address a sparse logical space and are only meaningful
 * when they come from `Append`.
 */
class StableByteArena
{
public:
    StableByteArena() = default;

    StableByteArena(const StableByteArena &) = delete;
    StableByteArena &operator=(const StableByteArena &) = delete;
    StableByteArena(StableByteArena &&) = delete;
    StableByteArena &operator=(StableByteArena &&) = delete;

    ~StableByteArena() = default;

    /**
     * @brief Copies @p bytes into contiguous storage.
     *
     * @param bytes Bytes to store.
     * @return Offset of the first stored byte.
     * @throws std::length_error when the logical space is exhausted.
     */
    uint64_t Append(std::string_view bytes);

    /**
     * @brief Grows the range at (@p offset, @p length) in place.
     *
     * Succeeds only when the range ends at the arena's tail and @p bytes fit
     * in the current block. Other readers never see a partial extension,
     * because they only read lengths they were given.
     *
     * @param offset Start of the range.
     * @param length Current length of the range.
     * @param bytes Bytes to add after the range.
     * @return Whether the range was extended.
     */
    bool TryExtend(uint64_t offset, uint64_t length, std::string_view bytes);

    /**
     * @brief Resolves a stored range.
     *
     * @param offset Offset returned by `Append`.
     * @param length Number of bytes.
     * @return The bytes, or an empty view when the range was never stored.
     */
    [[nodiscard]] std::string_view View(uint64_t offset, uint64_t length) const noexcept;

    /** @brief Returns the heap bytes reserved for stored data. */
    [[nodiscard]] size_t CapacityBytes() const noexcept;

private:
    static constexpr uint64_t SLOT_BYTES = uint64_t{1} << 20U;
    static constexpr size_t PAGE_SLOTS = 4096;
    static constexpr size_t TOP_PAGES = 4096;

    using Page = std::array<std::atomic<const char *>, PAGE_SLOTS>;

    /** @brief Points every slot of a new block at its bytes. */
    void PublishSlot(uint64_t slot, const char *bytes);

    mutable std::mutex mWriteLock;
    std::vector<std::unique_ptr<char[]>> mBlocks;
    std::vector<std::unique_ptr<Page>> mPages;
    /** @brief Next free logical offset. Guarded by `mWriteLock`. */
    uint64_t mTail = 0;
    /** @brief Logical end of the current block. Guarded by `mWriteLock`. */
    uint64_t mBlockEnd = 0;
    /** @brief Logical address of the next free byte inside the current block's memory. */
    char *mTailBytes = nullptr;
    std::atomic<size_t> mCapacityBytes{0};
    /** @brief Readers reject ranges past this value. */
    std::atomic<uint64_t> mPublishedEnd{0};
    std::array<std::atomic<Page *>, TOP_PAGES> mTop{};
};

} // namespace loglib::internal
