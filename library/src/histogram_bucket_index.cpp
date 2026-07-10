#include "loglib/histogram_bucket_index.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace loglib
{

namespace
{

/// Level -> array index. Debug asserts if the enum grows past the
/// slot count; release clamps rogue values to `Unknown` (slot 0)
/// rather than stomping the array.
constexpr size_t LEVEL_SLOT_COUNT = CANONICAL_LEVEL_COUNT + 1;

constexpr size_t LevelToSlot(LogLevel level) noexcept
{
    const auto raw = static_cast<size_t>(level);
    assert(raw < LEVEL_SLOT_COUNT && "LogLevel grew; extend HistogramBucketIndex::LEVEL_SLOT_COUNT");
    return raw < LEVEL_SLOT_COUNT ? raw : 0;
}

/// Floor of `numerator / denominator`. Plain integer division
/// truncates toward zero, which would put e.g. `-1 us / 1 s` into
/// bucket 0 rather than bucket -1. Explicit floor keeps bucket
/// truncation monotonic across the pre-1970 epoch boundary.
constexpr int64_t FloorDivide(int64_t numerator, int64_t denominator) noexcept
{
    const int64_t quotient = numerator / denominator;
    const int64_t remainder = numerator % denominator;
    // Signs disagree with a non-zero remainder: truncation is one above the floor.
    if ((remainder != 0) && ((remainder < 0) != (denominator < 0)))
    {
        return quotient - 1;
    }
    return quotient;
}

} // namespace

uint32_t LevelBucket::Total() const noexcept
{
    uint32_t total = 0;
    for (const uint32_t count : counts)
    {
        total += count;
    }
    return total;
}

std::string_view HistogramBucketSizeLabel(HistogramBucketSize size) noexcept
{
    using namespace std::string_view_literals;
    switch (size)
    {
    case HistogramBucketSize::OneSecond:
        return "1 s"sv;
    case HistogramBucketSize::TenSeconds:
        return "10 s"sv;
    case HistogramBucketSize::OneMinute:
        return "1 min"sv;
    case HistogramBucketSize::TenMinutes:
        return "10 min"sv;
    case HistogramBucketSize::OneHour:
        return "1 h"sv;
    case HistogramBucketSize::OneDay:
        return "1 d"sv;
    }
    // Unreachable given the closed enum. Assert in debug so a new
    // rung without a label is caught by tests; release falls back
    // to `"?"` rather than aborting the GUI paint.
    assert(false && "HistogramBucketSize rung is missing a label");
    return "?"sv;
}

HistogramBucketIndex::HistogramBucketIndex(HistogramBucketSize size) noexcept : mBucketSize(size)
{
}

TimeStamp HistogramBucketIndex::TruncateToBucket(TimeStamp ts, HistogramBucketSize size) noexcept
{
    const auto widthUs = HistogramBucketWidth(size).count();
    const int64_t rawUs = ts.time_since_epoch().count();
    const int64_t bucketIdx = FloorDivide(rawUs, widthUs);
    return TimeStamp{std::chrono::microseconds{bucketIdx * widthUs}};
}

HistogramBucketSize HistogramBucketIndex::AutoBucketSize(
    TimeStamp min, TimeStamp max, int visibleBucketBudget
) noexcept
{
    if (max < min)
    {
        std::swap(min, max);
    }
    // Degenerate (single-instant) range: finest rung is fine.
    if (max == min)
    {
        return HistogramBucketSize::OneSecond;
    }
    if (visibleBucketBudget <= 0)
    {
        return HistogramBucketSize::OneDay;
    }
    const auto spanUs = (max - min).count();
    for (uint8_t rung = 0; rung < HISTOGRAM_BUCKET_SIZE_COUNT; ++rung)
    {
        const auto size = static_cast<HistogramBucketSize>(rung);
        const auto widthUs = HistogramBucketWidth(size).count();
        // +1 fencepost: `[min, max]` inclusive spans up to
        // `ceil((max-min)/width) + 1` buckets in the worst case.
        const int64_t bucketCount = (spanUs / widthUs) + 1;
        if (bucketCount <= visibleBucketBudget)
        {
            return size;
        }
    }
    return HistogramBucketSize::OneDay;
}

void HistogramBucketIndex::AddRow(TimeStamp ts, LogLevel level)
{
    const auto widthUs = BucketWidth().count();
    const int64_t tsUs = ts.time_since_epoch().count();

    if (mBuckets.empty())
    {
        // Anchor origin at the bucket boundary covering the first row.
        mOrigin = TruncateToBucket(ts, mBucketSize);
        mBuckets.resize(1);
        mBuckets[0].counts[LevelToSlot(level)] += 1;
        mTotalRowCount += 1;
        mMinTimestamp = ts;
        mMaxTimestamp = ts;
        return;
    }

    const int64_t originUs = mOrigin.time_since_epoch().count();
    const int64_t delta = tsUs - originUs;
    int64_t bucketIdx = FloorDivide(delta, widthUs);

    if (bucketIdx < 0)
    {
        // Out-of-order backfill: shift right and rebase the origin.
        // Rare for append-only streams; O(N) in the bucket count.
        const auto shift = static_cast<size_t>(-bucketIdx);
        mBuckets.insert(mBuckets.begin(), shift, LevelBucket{});
        mOrigin = TimeStamp{std::chrono::microseconds{originUs + (bucketIdx * widthUs)}};
        bucketIdx = 0;
    }
    else if (static_cast<size_t>(bucketIdx) >= mBuckets.size())
    {
        mBuckets.resize(static_cast<size_t>(bucketIdx) + 1);
    }

    mBuckets[static_cast<size_t>(bucketIdx)].counts[LevelToSlot(level)] += 1;
    mTotalRowCount += 1;
    if (ts < mMinTimestamp)
    {
        mMinTimestamp = ts;
    }
    if (ts > mMaxTimestamp)
    {
        mMaxTimestamp = ts;
    }
}

void HistogramBucketIndex::Reset() noexcept
{
    mBuckets.clear();
    mOrigin = TimeStamp{};
    mTotalRowCount = 0;
    mMinTimestamp = TimeStamp{};
    mMaxTimestamp = TimeStamp{};
}

void HistogramBucketIndex::SetBucketSize(HistogramBucketSize size) noexcept
{
    if (size != mBucketSize)
    {
        mBucketSize = size;
        Reset();
    }
}

std::optional<size_t> HistogramBucketIndex::BucketOf(TimeStamp ts) const noexcept
{
    if (mBuckets.empty())
    {
        return std::nullopt;
    }
    const auto widthUs = BucketWidth().count();
    const int64_t delta = ts.time_since_epoch().count() - mOrigin.time_since_epoch().count();
    const int64_t bucketIdx = FloorDivide(delta, widthUs);
    if (bucketIdx < 0 || static_cast<size_t>(bucketIdx) >= mBuckets.size())
    {
        return std::nullopt;
    }
    return static_cast<size_t>(bucketIdx);
}

TimeStamp HistogramBucketIndex::BucketStart(size_t index) const noexcept
{
    const auto widthUs = BucketWidth().count();
    return TimeStamp{std::chrono::microseconds{
        mOrigin.time_since_epoch().count() + (static_cast<int64_t>(index) * widthUs)}};
}

TimeStamp HistogramBucketIndex::BucketEnd(size_t index) const noexcept
{
    return BucketStart(index) + BucketWidth();
}

} // namespace loglib
