/**
 * @brief Append-only byte store whose bytes never move.
 *
 * Writers take an internal lock. Readers take none, and a view returned from the arena stays valid for the arena's
 * whole lifetime. LogFile uses it to keep strings that cannot be views into the mapped file, so rows can safely refer
 * to them while parsing continues on other threads.
 *
 * This is part of the core module.
 */
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
 * @brief A store of bytes that only grows and never moves what it has stored.
 *
 * Writers take an internal mutex. View takes no lock and may run at the same time as writers, and a view it returns
 * stays valid until the arena is destroyed. Offsets belong to a sparse logical address space, and only values returned
 * by Append are meaningful.
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
     * @brief Copies bytes into the arena and returns the offset of the first one. Throws std::length_error when the
     * address space is used up.
     */
    uint64_t Append(std::string_view bytes);

    /**
     * @brief Adds bytes after a stored range, in place.
     *
     * It works only if the range ends at the end of the arena and the bytes fit in the current block. Readers never see
     * a half-extended range, because they read only the lengths they were given. Returns whether the range was
     * extended.
     */
    bool TryExtend(uint64_t offset, uint64_t length, std::string_view bytes);

    /**
     * @brief Returns the bytes of a stored range, or an empty view if the range was never stored. The offset must come
     * from Append.
     */
    [[nodiscard]] std::string_view View(uint64_t offset, uint64_t length) const noexcept;

    /** @brief The heap bytes reserved for stored data. */
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
    /** @brief Next free logical offset. Guarded by mWriteLock. */
    uint64_t mTail = 0;
    /** @brief Logical end of the current block. Guarded by mWriteLock. */
    uint64_t mBlockEnd = 0;
    /** @brief The memory address of the next free byte in the current block. */
    char *mTailBytes = nullptr;
    std::atomic<size_t> mCapacityBytes{0};
    /** @brief Readers reject ranges past this value. */
    std::atomic<uint64_t> mPublishedEnd{0};
    std::array<std::atomic<Page *>, TOP_PAGES> mTop{};
};

} // namespace loglib::internal
