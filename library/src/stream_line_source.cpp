#include "loglib/stream_line_source.hpp"

#include "loglib/bytes_producer.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace loglib
{

StreamLineSource::StreamLineSource(std::filesystem::path displayName, std::unique_ptr<BytesProducer> producer)
    : mDisplayName(std::move(displayName)), mProducer(std::move(producer))
{
}

StreamLineSource::~StreamLineSource() = default;

const std::filesystem::path &StreamLineSource::Path() const noexcept
{
    return mDisplayName;
}

std::string StreamLineSource::RawLine(size_t lineId) const
{
    std::lock_guard<std::mutex> guard(mLock);
    if (!LineIsLiveLocked(lineId))
    {
        throw std::out_of_range("StreamLineSource::RawLine: lineId " + std::to_string(lineId) + " is not available");
    }
    return mLines[IndexForLocked(lineId)];
}

std::string_view StreamLineSource::ResolveMmapBytes(
    uint64_t /*offset*/, uint32_t /*length*/, size_t /*lineId*/
) const noexcept
{
    // Stream sources never produce `MmapSlice` compact values. Returning
    // an empty view is the defensive answer if a stale value somehow
    // reaches us; under normal operation no caller gets here.
    return {};
}

std::string_view StreamLineSource::ResolveOwnedBytes(uint64_t offset, uint32_t length, size_t lineId) const noexcept
{
    std::lock_guard<std::mutex> guard(mLock);
    if (!LineIsLiveLocked(lineId))
    {
        return {};
    }
    const std::string &arena = mLineOwnedBytes[IndexForLocked(lineId)];
    if (offset > arena.size() || offset + length > arena.size())
    {
        return {};
    }
    // The deque entry is stable across concurrent `AppendLine`s for as
    // long as it isn't evicted, so the returned view remains valid past
    // the lock release.  asks callers not to retain it past
    // the next `EvictBefore` for the same line id.
    return std::string_view(arena.data() + offset, length);
}

std::span<const char> StreamLineSource::StableBytes() const noexcept
{
    // Stream sources never expose stable bytes: the underlying byte
    // producer's buffers are reused across reads. Every string-shaped
    // value must land in the per-line owned arena.
    return {};
}

uint64_t StreamLineSource::AppendOwnedBytes(size_t lineId, std::string_view bytes)
{
    std::lock_guard<std::mutex> guard(mLock);
    if (!LineIsLiveLocked(lineId))
    {
        throw std::out_of_range(
            "StreamLineSource::AppendOwnedBytes: lineId " + std::to_string(lineId) + " is not available"
        );
    }
    std::string &arena = mLineOwnedBytes[IndexForLocked(lineId)];
    const auto offset = static_cast<uint64_t>(arena.size());
    arena.append(bytes.data(), bytes.size());
    return offset;
}

bool StreamLineSource::SupportsEviction() const noexcept
{
    return true;
}

void StreamLineSource::EvictBefore(size_t firstSurvivingLineId)
{
    std::lock_guard<std::mutex> guard(mLock);
    if (firstSurvivingLineId <= mFirstAvailableLineId)
    {
        return;
    }
    // Cap at `mNextLineId` so a caller asking to evict past the tail
    // simply drops everything held; subsequent `AppendLine` calls
    // resume at `mNextLineId` (advancing `mFirstAvailableLineId` to
    // match would also be valid but loses the diagnostic that the
    // caller over-shot, so we clamp instead).
    const size_t target = std::min(firstSurvivingLineId, mNextLineId);
    const size_t drop = target - mFirstAvailableLineId;
    for (size_t i = 0; i < drop; ++i)
    {
        mLines.pop_front();
        mLineOwnedBytes.pop_front();
    }
    mFirstAvailableLineId = target;
}

size_t StreamLineSource::FirstAvailableLineId() const noexcept
{
    std::lock_guard<std::mutex> guard(mLock);
    return mFirstAvailableLineId;
}

BytesProducer *StreamLineSource::Producer() noexcept
{
    return mProducer.get();
}

const BytesProducer *StreamLineSource::Producer() const noexcept
{
    return mProducer.get();
}

size_t StreamLineSource::AppendLine(std::string rawLine, std::string ownedBytes)
{
    std::lock_guard<std::mutex> guard(mLock);
    const size_t lineId = mNextLineId++;
    mLines.push_back(std::move(rawLine));
    mLineOwnedBytes.push_back(std::move(ownedBytes));
    return lineId;
}

size_t StreamLineSource::Size() const noexcept
{
    std::lock_guard<std::mutex> guard(mLock);
    return mLines.size();
}

size_t StreamLineSource::OwnedMemoryBytes() const noexcept
{
    std::lock_guard<std::mutex> guard(mLock);
    // `std::deque` doesn't expose `capacity()` — it allocates in
    // chunks of an implementation-defined block size. Fall back to a
    // size-based estimate for the per-string overhead and accumulate
    // the actual capacities of each entry.
    size_t total = (mLines.size() + mLineOwnedBytes.size()) * sizeof(std::string);
    for (const auto &line : mLines)
    {
        total += line.capacity();
    }
    for (const auto &arena : mLineOwnedBytes)
    {
        total += arena.capacity();
    }
    return total;
}

bool StreamLineSource::LineIsLiveLocked(size_t lineId) const noexcept
{
    return lineId >= mFirstAvailableLineId && lineId < mNextLineId;
}

size_t StreamLineSource::IndexForLocked(size_t lineId) const noexcept
{
    return lineId - mFirstAvailableLineId;
}

} // namespace loglib
