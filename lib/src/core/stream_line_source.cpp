#include "loglib/core/stream_line_source.hpp"

#include "loglib/core/bytes_producer.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace loglib
{

StreamLineSource::StreamLineSource(std::filesystem::path displayName, std::unique_ptr<BytesProducer> producer)
    : LineSource(false), mDisplayName(std::move(displayName)), mProducer(std::move(producer))
{
}

StreamLineSource::~StreamLineSource() = default;

const std::filesystem::path &StreamLineSource::Path() const noexcept
{
    return mDisplayName;
}

std::string StreamLineSource::RawLine(size_t lineId) const
{
    const std::scoped_lock guard(mLock);
    if (!LineIsLiveLocked(lineId))
    {
        throw std::out_of_range("StreamLineSource::RawLine: lineId " + std::to_string(lineId) + " is not available");
    }
    return mLines[IndexForLocked(lineId)];
}

void StreamLineSource::AppendRawLine(size_t lineId, std::string &out) const
{
    const std::scoped_lock guard(mLock);
    if (!LineIsLiveLocked(lineId))
    {
        throw std::out_of_range("StreamLineSource::RawLine: lineId " + std::to_string(lineId) + " is not available");
    }
    out.append(mLines[IndexForLocked(lineId)]);
}

std::string_view StreamLineSource::ResolveMmapBytes(uint64_t offset, uint32_t length, size_t lineId) const noexcept
{
    const std::scoped_lock guard(mLock);
    if (!LineIsLiveLocked(lineId))
    {
        return {};
    }
    const std::string &line = mLines[IndexForLocked(lineId)];
    if (offset > line.size() || offset + length > line.size())
    {
        return {};
    }
    // Deque entries keep their address until they are evicted, so the view outlives the lock. Callers must not keep it
    // past the next EvictBefore for this line.
    return {line.data() + offset, length};
}

std::string_view StreamLineSource::ResolveOwnedBytes(uint64_t offset, uint32_t length, size_t lineId) const noexcept
{
    const std::scoped_lock guard(mLock);
    if (!LineIsLiveLocked(lineId))
    {
        return {};
    }
    const std::string &arena = mLineOwnedBytes[IndexForLocked(lineId)];
    if (offset > arena.size() || offset + length > arena.size())
    {
        return {};
    }
    // Deque entries keep their address until they are evicted, so the view outlives the lock. Callers must not keep it
    // past the next EvictBefore for this line.
    return {arena.data() + offset, length};
}

std::span<const char> StreamLineSource::StableBytes() const noexcept
{
    // No stable bytes: producer buffers are reused; everything must
    // land in the per-line owned arena.
    return {};
}

uint64_t StreamLineSource::AppendOwnedBytes(size_t lineId, std::string_view bytes)
{
    const std::scoped_lock guard(mLock);
    if (!LineIsLiveLocked(lineId))
    {
        throw std::out_of_range(
            "StreamLineSource::AppendOwnedBytes: lineId " + std::to_string(lineId) + " is not available"
        );
    }
    std::string &arena = mLineOwnedBytes[IndexForLocked(lineId)];
    const uint64_t offset = arena.size();
    arena.append(bytes.data(), bytes.size());
    return offset;
}

bool StreamLineSource::SupportsEviction() const noexcept
{
    return true;
}

void StreamLineSource::EvictBefore(size_t firstSurvivingLineId)
{
    const std::scoped_lock guard(mLock);
    if (firstSurvivingLineId <= mFirstAvailableLineId)
    {
        return;
    }
    // A target past the next line id drops everything held, and AppendLine continues at the next line id.
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
    const std::scoped_lock guard(mLock);
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

size_t StreamLineSource::DroppedByteCount() const noexcept
{
    return mProducer ? mProducer->DroppedByteCount() : 0;
}

size_t StreamLineSource::AppendLine(std::string rawLine, std::string ownedBytes)
{
    const std::scoped_lock guard(mLock);
    const size_t lineId = mNextLineId++;
    mLines.push_back(std::move(rawLine));
    mLineOwnedBytes.push_back(std::move(ownedBytes));
    return lineId;
}

void StreamLineSource::UnreadBytes(std::string bytes)
{
    const std::scoped_lock guard(mLock);
    bytes.append(mUnreadBytes);
    mUnreadBytes = std::move(bytes);
}

std::string StreamLineSource::TakeUnreadBytes()
{
    const std::scoped_lock guard(mLock);
    return std::exchange(mUnreadBytes, std::string{});
}

size_t StreamLineSource::Size() const noexcept
{
    const std::scoped_lock guard(mLock);
    return mLines.size();
}

size_t StreamLineSource::OwnedMemoryBytes() const noexcept
{
    const std::scoped_lock guard(mLock);
    // A lower bound: the string objects and the capacity of each string. The deque's own blocks, the slack of short
    // strings and the members of this object are not counted. To see whether a stream keeps growing, a lower bound is
    // more useful than an estimate that mixes allocator rounding with real growth.
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
