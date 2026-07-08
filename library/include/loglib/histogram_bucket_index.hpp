#pragma once

#include "loglib/log_level.hpp"
#include "loglib/log_value.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace loglib
{

/// Fixed-width bucket rungs on the auto-zoom ladder. Kept small so a
/// `switch` on the enum inlines to a single divisor in the hot path.
/// The ladder matches lnav's `z` / `Shift+Z` zoom set:
/// 1 s, 10 s, 1 min, 10 min, 1 h, 1 d.
enum class HistogramBucketSize : uint8_t
{
    OneSecond = 0,
    TenSeconds,
    OneMinute,
    TenMinutes,
    OneHour,
    OneDay,
};

/// Number of rungs on `HistogramBucketSize` (OneSecond..OneDay).
inline constexpr size_t HISTOGRAM_BUCKET_SIZE_COUNT = 6;

/// Microsecond width of one bucket at @p size. Constexpr so callers
/// can use it as a divisor without a runtime `switch`.
[[nodiscard]] constexpr std::chrono::microseconds HistogramBucketWidth(HistogramBucketSize size) noexcept
{
    using namespace std::chrono;
    switch (size)
    {
    case HistogramBucketSize::OneSecond:
        return duration_cast<microseconds>(seconds{1});
    case HistogramBucketSize::TenSeconds:
        return duration_cast<microseconds>(seconds{10});
    case HistogramBucketSize::OneMinute:
        return duration_cast<microseconds>(minutes{1});
    case HistogramBucketSize::TenMinutes:
        return duration_cast<microseconds>(minutes{10});
    case HistogramBucketSize::OneHour:
        return duration_cast<microseconds>(hours{1});
    case HistogramBucketSize::OneDay:
        return duration_cast<microseconds>(hours{24});
    }
    return microseconds{1};
}

/// Short human label, e.g. `"1 s"`, `"10 min"`, `"1 h"`. Used by the
/// widget's subtitle line and by tests.
[[nodiscard]] std::string_view HistogramBucketSizeLabel(HistogramBucketSize size) noexcept;

/// Per-bucket counts, one slot per canonical `LogLevel`. Slot 0
/// (`Unknown`) is included so rows with no resolvable level still
/// contribute to the total (rendered in the theme's default level
/// colour).
struct LevelBucket
{
    /// Indexed by `static_cast<size_t>(LogLevel)`; size is
    /// `CANONICAL_LEVEL_COUNT + 1` (Unknown..Fatal).
    std::array<uint32_t, CANONICAL_LEVEL_COUNT + 1> counts{};

    [[nodiscard]] uint32_t Total() const noexcept;
};

/// Dense time-bucket index over `(TimeStamp, LogLevel)` events. Zero
/// Qt dependency — feeds the GUI's `HistogramWidget` and any headless
/// consumer.
///
/// Buckets are indexed by microseconds since `Origin()`, truncated to
/// `BucketWidth()`. `AddRow` extends the vector as needed; out-of-order
/// timestamps *before* the origin shift the vector once (rare in
/// append-only log streams; O(N) in current bucket count when it does
/// fire, so keep it in mind if a source produces heavy backfill).
class HistogramBucketIndex
{
public:
    HistogramBucketIndex() = default;

    /// Pin the bucket rung. Does not clear existing buckets — use
    /// `Reset()` before re-feeding when changing zoom.
    explicit HistogramBucketIndex(HistogramBucketSize size) noexcept;

    /// Contribute one row (its timestamp + resolved level) to the
    /// bucket containing @p ts. Unknown level goes to slot 0.
    void AddRow(TimeStamp ts, LogLevel level);

    /// Drop all buckets, keep bucket-size. Origin becomes unset again;
    /// the next `AddRow` re-anchors it.
    void Reset() noexcept;

    /// Change the bucket rung. Also `Reset()`s — the caller is expected
    /// to re-feed rows from `LogModel`.
    void SetBucketSize(HistogramBucketSize size) noexcept;

    /// Pick the coarsest rung on the ladder whose bucket count over
    /// `[min, max]` stays under @p visibleBucketBudget. If @p min ==
    /// @p max returns `OneSecond`. If the range would still exceed the
    /// budget at `OneDay`, returns `OneDay` (the caller decides how to
    /// render a very wide range).
    [[nodiscard]] static HistogramBucketSize AutoBucketSize(
        TimeStamp min, TimeStamp max, int visibleBucketBudget = 500
    ) noexcept;

    /// Truncate @p ts down to the boundary of a @p size bucket. Public
    /// so the widget can align its axis labels.
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

    /// Timestamp of the first bucket's *start*. Only meaningful when
    /// `!Empty()`.
    [[nodiscard]] TimeStamp Origin() const noexcept
    {
        return mOrigin;
    }

    [[nodiscard]] bool Empty() const noexcept
    {
        return mBuckets.empty();
    }

    /// Total rows fed via `AddRow` (matches the sum of all bucket
    /// totals). O(1).
    [[nodiscard]] uint64_t TotalRowCount() const noexcept
    {
        return mTotalRowCount;
    }

    /// Bucket index containing @p ts, or `nullopt` when out of the
    /// current `[Origin, Origin + N * width)` range.
    [[nodiscard]] std::optional<size_t> BucketOf(TimeStamp ts) const noexcept;

    /// Start (inclusive) of bucket @p index. UB if `index >=
    /// Buckets().size()`.
    [[nodiscard]] TimeStamp BucketStart(size_t index) const noexcept;

    /// End (exclusive) of bucket @p index. UB if `index >=
    /// Buckets().size()`.
    [[nodiscard]] TimeStamp BucketEnd(size_t index) const noexcept;

    /// Precise minimum / maximum timestamp observed via `AddRow`,
    /// tracked in O(1) per row. Distinct from `BucketStart(0)` /
    /// `BucketEnd(last)` (which snap to bucket boundaries) so a
    /// caller like `AutoBucketSize` sees the *true* span, not one
    /// inflated by up to `BucketWidth()`. `nullopt` when empty.
    [[nodiscard]] std::optional<TimeStamp> MinTimestamp() const noexcept
    {
        return mBuckets.empty() ? std::nullopt : std::optional<TimeStamp>{mMinTimestamp};
    }
    [[nodiscard]] std::optional<TimeStamp> MaxTimestamp() const noexcept
    {
        return mBuckets.empty() ? std::nullopt : std::optional<TimeStamp>{mMaxTimestamp};
    }

private:
    HistogramBucketSize mBucketSize = HistogramBucketSize::OneMinute;
    /// Start of the first bucket. Meaningful only when `!mBuckets.empty()`.
    TimeStamp mOrigin{};
    std::vector<LevelBucket> mBuckets;
    uint64_t mTotalRowCount = 0;
    /// Precise observed range. Updated in `AddRow`; meaningful only
    /// when `!mBuckets.empty()`. Cleared by `Reset()`.
    TimeStamp mMinTimestamp{};
    TimeStamp mMaxTimestamp{};
};

} // namespace loglib
