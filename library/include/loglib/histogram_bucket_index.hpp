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

/// Fixed-width bucket rungs on the auto-zoom ladder. Matches lnav's
/// `z` / `Shift+Z` set: 1 s, 10 s, 1 min, 10 min, 1 h, 1 d. Kept
/// small so a `switch` inlines to one divisor in the hot path.
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

/// Named ladder step. `TenSeconds` / `TenMinutes` step by
/// `TEN_UNITS_PER_STRIDE` from their neighbours; `OneDay` groups
/// `HOURS_PER_DAY` hours. Pulled out so clang-tidy's
/// `cppcoreguidelines-avoid-magic-numbers` doesn't fire on the
/// switch below, and so the constants document intent inline.
inline constexpr int TEN_UNITS_PER_STRIDE = 10;
inline constexpr int HOURS_PER_DAY = 24;

/// Microsecond width of one bucket at @p size.
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

/// Short human label, e.g. `"1 s"`, `"10 min"`, `"1 h"`. Used in the
/// widget's details strip and by tests.
[[nodiscard]] std::string_view HistogramBucketSizeLabel(HistogramBucketSize size) noexcept;

/// Per-bucket counts, one slot per canonical `LogLevel`. Slot 0
/// (`Unknown`) is included so unresolved-level rows still contribute
/// to the total. Aggregate by design so `HistogramBucketIndex` can
/// index into it directly; the public array is intentional.
struct LevelBucket
{
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    /// Indexed by `static_cast<size_t>(LogLevel)`; size is
    /// `CANONICAL_LEVEL_COUNT + 1` (Unknown..Fatal).
    std::array<uint32_t, CANONICAL_LEVEL_COUNT + 1> counts{};
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] uint32_t Total() const noexcept;
};

/// Dense time-bucket index over `(TimeStamp, LogLevel)` events. No Qt
/// dependency — feeds the GUI widget and headless consumers alike.
///
/// Buckets are indexed by microseconds since `Origin()`, truncated to
/// `BucketWidth()`. `AddRow` extends the vector on demand; timestamps
/// before the origin shift the vector once (O(N) in current bucket
/// count), which is fine in append-only streams but worth knowing for
/// sources that produce heavy backfill.
class HistogramBucketIndex
{
public:
    HistogramBucketIndex() = default;

    /// Pin the bucket rung. Does not clear existing buckets; call
    /// `Reset()` first when changing zoom on a live index.
    explicit HistogramBucketIndex(HistogramBucketSize size) noexcept;

    /// Contribute one row to the bucket containing @p ts. Unknown
    /// level goes to slot 0.
    void AddRow(TimeStamp ts, LogLevel level);

    /// Drop all buckets, keep the bucket rung. The next `AddRow`
    /// re-anchors the origin.
    void Reset() noexcept;

    /// Change the bucket rung. Also `Reset()`s; the caller is
    /// expected to re-feed rows.
    void SetBucketSize(HistogramBucketSize size) noexcept;

    /// Default column budget passed to `AutoBucketSize` from the
    /// widget. ~500 columns give a bar-per-pixel-ish density on a
    /// typical bottom-docked strip; tuned once and named here so the
    /// value doesn't reappear as a magic literal.
    static constexpr int DEFAULT_VISIBLE_BUCKET_BUDGET = 500;

    /// Coarsest rung whose bucket count over `[min, max]` stays under
    /// @p visibleBucketBudget. Returns `OneSecond` for a zero-width
    /// range and `OneDay` when even a day-wide rung would exceed the
    /// budget (caller decides how to render very wide ranges).
    [[nodiscard]] static HistogramBucketSize AutoBucketSize(
        TimeStamp min, TimeStamp max, int visibleBucketBudget = DEFAULT_VISIBLE_BUCKET_BUDGET
    ) noexcept;

    /// Truncate @p ts down to a @p size bucket boundary. Public so
    /// the widget can align its axis labels.
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

    /// Start of the first bucket. Only meaningful when `!Empty()`.
    [[nodiscard]] TimeStamp Origin() const noexcept
    {
        return mOrigin;
    }

    [[nodiscard]] bool Empty() const noexcept
    {
        return mBuckets.empty();
    }

    /// Total rows fed via `AddRow` (sum of all bucket totals). O(1).
    [[nodiscard]] uint64_t TotalRowCount() const noexcept
    {
        return mTotalRowCount;
    }

    /// Bucket index containing @p ts, or `nullopt` when out of the
    /// current `[Origin, Origin + N * width)` range.
    [[nodiscard]] std::optional<size_t> BucketOf(TimeStamp ts) const noexcept;

    /// Start (inclusive) of bucket @p index. UB when @p index is out
    /// of range.
    [[nodiscard]] TimeStamp BucketStart(size_t index) const noexcept;

    /// End (exclusive) of bucket @p index. UB when @p index is out
    /// of range.
    [[nodiscard]] TimeStamp BucketEnd(size_t index) const noexcept;

    /// Precise min / max timestamp observed via `AddRow`, tracked in
    /// O(1) per row. Distinct from `BucketStart(0)` / `BucketEnd(last)`
    /// (which snap to boundaries) so callers like `AutoBucketSize`
    /// see the true span, not one inflated by up to `BucketWidth()`.
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
    /// Start of bucket 0; only meaningful when `!mBuckets.empty()`.
    /// `TimeStamp` is a chrono `time_point`, which value-initialises
    /// to the epoch on default construction; the explicit brace
    /// initialiser was flagged as redundant by clang-tidy.
    TimeStamp mOrigin;
    std::vector<LevelBucket> mBuckets;
    uint64_t mTotalRowCount = 0;
    /// Precise observed range; only meaningful when `!mBuckets.empty()`.
    TimeStamp mMinTimestamp;
    TimeStamp mMaxTimestamp;
};

} // namespace loglib
