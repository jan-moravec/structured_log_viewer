#include "loglib/internal/stable_byte_arena.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace loglib::internal
{

void StableByteArena::PublishSlot(uint64_t slot, const char *bytes)
{
    const auto pageIndex = static_cast<size_t>(slot / PAGE_SLOTS);
    if (pageIndex >= TOP_PAGES)
    {
        throw std::length_error("StableByteArena: logical space exhausted");
    }
    Page *page = mTop[pageIndex].load(std::memory_order_relaxed);
    if (page == nullptr)
    {
        mPages.push_back(std::make_unique<Page>());
        page = mPages.back().get();
        mTop[pageIndex].store(page, std::memory_order_release);
    }
    (*page)[static_cast<size_t>(slot % PAGE_SLOTS)].store(bytes, std::memory_order_release);
}

uint64_t StableByteArena::Append(std::string_view bytes)
{
    const std::scoped_lock lock(mWriteLock);
    if (bytes.empty())
    {
        return mTail;
    }
    if (bytes.size() > mBlockEnd - mTail)
    {
        // A block spans whole slots, so one append stays contiguous.
        const uint64_t slots = std::max<uint64_t>(1, (bytes.size() + SLOT_BYTES - 1) / SLOT_BYTES);
        const auto capacity = static_cast<size_t>(slots * SLOT_BYTES);
        mBlocks.push_back(std::make_unique_for_overwrite<char[]>(capacity));
        char *block = mBlocks.back().get();
        const uint64_t firstSlot = mBlockEnd / SLOT_BYTES;
        for (uint64_t i = 0; i < slots; ++i)
        {
            PublishSlot(firstSlot + i, block + static_cast<size_t>(i * SLOT_BYTES));
        }
        mTail = mBlockEnd;
        mBlockEnd += slots * SLOT_BYTES;
        mTailBytes = block;
        mCapacityBytes.fetch_add(capacity, std::memory_order_relaxed);
    }
    const uint64_t offset = mTail;
    std::memcpy(mTailBytes, bytes.data(), bytes.size());
    mTailBytes += bytes.size();
    mTail += bytes.size();
    mPublishedEnd.store(mTail, std::memory_order_release);
    return offset;
}

bool StableByteArena::TryExtend(uint64_t offset, uint64_t length, std::string_view bytes)
{
    const std::scoped_lock lock(mWriteLock);
    if (offset + length != mTail || bytes.size() > mBlockEnd - mTail)
    {
        return false;
    }
    if (!bytes.empty())
    {
        std::memcpy(mTailBytes, bytes.data(), bytes.size());
        mTailBytes += bytes.size();
        mTail += bytes.size();
        mPublishedEnd.store(mTail, std::memory_order_release);
    }
    return true;
}

std::string_view StableByteArena::View(uint64_t offset, uint64_t length) const noexcept
{
    if (length == 0 || offset > mPublishedEnd.load(std::memory_order_acquire) ||
        length > mPublishedEnd.load(std::memory_order_acquire) - offset)
    {
        return {};
    }
    const uint64_t slot = offset / SLOT_BYTES;
    const auto pageIndex = static_cast<size_t>(slot / PAGE_SLOTS);
    if (pageIndex >= TOP_PAGES)
    {
        return {};
    }
    const Page *page = mTop[pageIndex].load(std::memory_order_acquire);
    if (page == nullptr)
    {
        return {};
    }
    const char *base = (*page)[static_cast<size_t>(slot % PAGE_SLOTS)].load(std::memory_order_acquire);
    if (base == nullptr)
    {
        return {};
    }
    return {base + static_cast<size_t>(offset % SLOT_BYTES), static_cast<size_t>(length)};
}

size_t StableByteArena::CapacityBytes() const noexcept
{
    return mCapacityBytes.load(std::memory_order_relaxed);
}

} // namespace loglib::internal
