/**
 * @brief Counts rows per time bucket for the histogram.
 *
 * Rows are grouped into fixed-width time buckets from one second up to one day. The index can pick a bucket size that
 * fits the time range of the data and counts rows per bucket. The interface uses it to draw the histogram.
 *
 * This is part of the query module.
 */
#pragma once

#include "loglib/core/log_level.hpp"
#include "loglib/core/log_value.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace loglib
{

/**
 * @brief The bucket widths a histogram can use, from 1 second to 1 day. The set is small so that picking a width
 * compiles to one divisor on the hot path.
 */
enum class HistogramBucketSize : uint8_t
{
    OneSecond = 0,
    TenSeconds,
    OneMinute,
    TenMinutes,
    OneHour,
    OneDay,
};

/** @brief Number of bucket widths in HistogramBucketSize. */
inline constexpr size_t HISTOGRAM_BUCKET_SIZE_COUNT = 6;

/**
 * @brief The most buckets a HistogramBucketIndex holds. A row that would need more makes the buckets wider, so one
 * stray timestamp cannot use up memory.
 */
inline constexpr size_t HISTOGRAM_MAX_BUCKETS = size_t{1} << 20U;

/** @brief The step from one second to ten seconds and from one minute to ten minutes. */
inline constexpr int TEN_UNITS_PER_STRIDE = 10;

/** @brief The number of hours in the OneDay bucket. */
inline constexpr int HOURS_PER_DAY = 24;

/** @brief The width of one bucket of the given size. */
[[nodiscard]] constexpr std::chrono::microseconds HistogramBucketWidth(HistogramBucketSize size) noexcept
{
    using namespace std::chrono;
    switch (size)
    {
    case HistogramBucketSize::OneSecond:
        return duration_cast<microseconds>(seconds{1});
    case HistogramBucketSize::TenSeconds:
        return duration_cast<microseconds>(seconds{TEN_UNITS_PER_STRIDE});
    case HistogramBucketSize::OneMinute:
        return duration_cast<microseconds>(minutes{1});
    case HistogramBucketSize::TenMinutes:
        return duration_cast<microseconds>(minutes{TEN_UNITS_PER_STRIDE});
    case HistogramBucketSize::OneHour:
        return duration_cast<microseconds>(hours{1});
    case HistogramBucketSize::OneDay:
        return duration_cast<microseconds>(hours{HOURS_PER_DAY});
    }
    return microseconds{1};
}

/** @brief A short label for display, such as "1 s", "10 min" or "1 h". */
[[nodiscard]] std::string_view HistogramBucketSizeLabel(HistogramBucketSize size) noexcept;

/**
 * @brief The row counts of one bucket, with one count per level.
 *
 * Rows with an unknown level are counted too, so they still add to the total.
 */
struct LevelBucket
{
    /** @brief One count per level, indexed by the LogLevel value. Unknown is at index 0, followed by Trace to Fatal. */
    std::array<uint32_t, CANONICAL_LEVEL_COUNT + 1> counts{}; // NOLINT(misc-non-private-member-variables-in-classes)

    [[nodiscard]] uint32_t Total() const noexcept;
};

/**
 * @brief Counts rows per time bucket and per level, for a histogram of log activity.
 *
 * It does not depend on Qt, so a graphical widget and command-line tools can both use it. Bucket i covers the time from
 * Origin plus i widths, and each width is BucketWidth. AddRow grows the bucket list as needed. A timestamp before the
 * origin shifts all buckets once, which is cheap for append-only streams but costly for sources that deliver old rows
 * late.
 *
 * A row that would need more than HISTOGRAM_MAX_BUCKETS buckets makes the index switch to the next wider size that
 * covers it, and BucketSize reports the new size. Beyond the widest size the row is counted in the nearest bucket.
 */
class HistogramBucketIndex
{
public:
    HistogramBucketIndex() = default;

    /**
     * @brief Creates an index with the given bucket width. Existing buckets are kept, so call Reset first when
     * changing the width of an index that holds rows.
     */
    explicit HistogramBucketIndex(HistogramBucketSize size) noexcept;

    /** @brief Counts one row in the bucket that contains ts. A row with an unknown level goes in the Unknown count. */
    void AddRow(TimeStamp ts, LogLevel level);

    /** @brief Removes all buckets but keeps the bucket width. The next AddRow sets the origin again. */
    void Reset() noexcept;

    /** @brief Changes the bucket width. A different width clears the buckets, so the caller must add the rows again. */
    void SetBucketSize(HistogramBucketSize size) noexcept;

    /**
     * @brief The number of buckets AutoBucketSize aims for by default. About 500 gives roughly one bar per pixel on a
     * wide, short strip.
     */
    static constexpr int DEFAULT_VISIBLE_BUCKET_BUDGET = 500;

    /**
     * @brief Picks the finest bucket size for which the range from min to max needs no more than visibleBucketBudget
     * buckets.
     *
     * A range with no width gives OneSecond. If even OneDay needs more buckets than the budget, or the budget is not
     * positive, the result is OneDay, and the caller decides how to show such a wide range. A min later than max is
     * swapped.
     */
    [[nodiscard]] static HistogramBucketSize AutoBucketSize(
        TimeStamp min, TimeStamp max, int visibleBucketBudget = DEFAULT_VISIBLE_BUCKET_BUDGET
    ) noexcept;

    /**
     * @brief Rounds ts down to the start of its bucket of the given size. Useful for placing axis labels on bucket
     * edges.
     */
    [[nodiscard]] static TimeStamp TruncateToBucket(TimeStamp ts, HistogramBucketSize size) noexcept;

    [[nodiscard]] HistogramBucketSize BucketSize() const noexcept
    {
        return mBucketSize;
    }

    [[nodiscard]] std::chrono::microseconds BucketWidth() const noexcept
    {
        return HistogramBucketWidth(mBucketSize);
    }

    [[nodiscard]] std::span<const LevelBucket> Buckets() const noexcept
    {
        return {mBuckets.data(), mBuckets.size()};
    }

    /** @brief The start of the first bucket. Only valid when the index is not empty. */
    [[nodiscard]] TimeStamp Origin() const noexcept
    {
        return mOrigin;
    }

    [[nodiscard]] bool Empty() const noexcept
    {
        return mBuckets.empty();
    }

    /** @brief The number of rows added, which is the sum of all buckets. Constant time. */
    [[nodiscard]] uint64_t TotalRowCount() const noexcept
    {
        return mTotalRowCount;
    }

    /** @brief The index of the bucket that contains ts, or nullopt if ts is outside the current buckets. */
    [[nodiscard]] std::optional<size_t> BucketOf(TimeStamp ts) const noexcept;

    /** @brief The start of a bucket, included in it. The index must be valid; anything else is undefined behavior. */
    [[nodiscard]] TimeStamp BucketStart(size_t index) const noexcept;

    /** @brief The end of a bucket, not included in it. The index must be valid; anything else is undefined behavior. */
    [[nodiscard]] TimeStamp BucketEnd(size_t index) const noexcept;

    /**
     * @brief The earliest and the latest timestamp added so far, or nullopt while the index is empty.
     *
     * They are exact. The edges of the first and last bucket are rounded to bucket boundaries and can be up to one
     * bucket width away, which would make AutoBucketSize see a wider span than the data has.
     */
    [[nodiscard]] std::optional<TimeStamp> MinTimestamp() const noexcept
    {
        return mBuckets.empty() ? std::nullopt : std::optional<TimeStamp>{mMinTimestamp};
    }
    [[nodiscard]] std::optional<TimeStamp> MaxTimestamp() const noexcept
    {
        return mBuckets.empty() ? std::nullopt : std::optional<TimeStamp>{mMaxTimestamp};
    }

private:
    /** @brief Switches to the next wider bucket size and merges the existing buckets. */
    void WidenBuckets();

    HistogramBucketSize mBucketSize = HistogramBucketSize::OneMinute;

    /** @brief The start of bucket 0. Only valid while there are buckets. */
    TimeStamp mOrigin;
    std::vector<LevelBucket> mBuckets;
    uint64_t mTotalRowCount = 0;

    /** @brief The exact range of the timestamps added. Only valid while there are buckets. */
    TimeStamp mMinTimestamp;
    TimeStamp mMaxTimestamp;
};

} // namespace loglib
