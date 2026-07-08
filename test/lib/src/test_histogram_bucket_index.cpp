#include <loglib/histogram_bucket_index.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_value.hpp>

#include <catch2/catch_all.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

using namespace loglib;
using namespace std::chrono_literals;

namespace
{

// 2026-01-01T00:00:00Z as a system_clock time_point. All fixtures
// derive their timestamps from this so the tests are TZ-agnostic.
constexpr TimeStamp Base()
{
    constexpr auto epochSeconds = std::chrono::seconds{1767225600};
    return TimeStamp{std::chrono::duration_cast<std::chrono::microseconds>(epochSeconds)};
}

TimeStamp At(TimeStamp base, std::chrono::microseconds offset)
{
    return base + offset;
}

} // namespace

TEST_CASE("HistogramBucketSizeLabel is defined for every rung", "[histogram_bucket_index]")
{
    CHECK(HistogramBucketSizeLabel(HistogramBucketSize::OneSecond) == "1 s");
    CHECK(HistogramBucketSizeLabel(HistogramBucketSize::TenSeconds) == "10 s");
    CHECK(HistogramBucketSizeLabel(HistogramBucketSize::OneMinute) == "1 min");
    CHECK(HistogramBucketSizeLabel(HistogramBucketSize::TenMinutes) == "10 min");
    CHECK(HistogramBucketSizeLabel(HistogramBucketSize::OneHour) == "1 h");
    CHECK(HistogramBucketSizeLabel(HistogramBucketSize::OneDay) == "1 d");
}

TEST_CASE("HistogramBucketWidth matches the ladder", "[histogram_bucket_index]")
{
    CHECK(HistogramBucketWidth(HistogramBucketSize::OneSecond) == std::chrono::seconds{1});
    CHECK(HistogramBucketWidth(HistogramBucketSize::TenSeconds) == std::chrono::seconds{10});
    CHECK(HistogramBucketWidth(HistogramBucketSize::OneMinute) == std::chrono::minutes{1});
    CHECK(HistogramBucketWidth(HistogramBucketSize::TenMinutes) == std::chrono::minutes{10});
    CHECK(HistogramBucketWidth(HistogramBucketSize::OneHour) == std::chrono::hours{1});
    CHECK(HistogramBucketWidth(HistogramBucketSize::OneDay) == std::chrono::hours{24});
}

TEST_CASE("HistogramBucketIndex is empty on construction", "[histogram_bucket_index]")
{
    HistogramBucketIndex index;
    CHECK(index.Empty());
    CHECK(index.Buckets().empty());
    CHECK(index.TotalRowCount() == 0);
    CHECK(index.BucketSize() == HistogramBucketSize::OneMinute);
}

TEST_CASE("AddRow anchors origin at the truncated first timestamp", "[histogram_bucket_index]")
{
    HistogramBucketIndex index{HistogramBucketSize::OneMinute};
    const TimeStamp firstTs = At(Base(), 42s + 500ms);

    index.AddRow(firstTs, LogLevel::Info);
    CHECK(index.Buckets().size() == 1);
    CHECK(index.TotalRowCount() == 1);
    // Origin is truncated to the enclosing bucket boundary.
    CHECK(index.Origin() == Base());
    CHECK(index.BucketStart(0) == Base());
    CHECK(index.BucketEnd(0) == Base() + std::chrono::minutes{1});
    CHECK(index.Buckets()[0].counts[static_cast<size_t>(LogLevel::Info)] == 1);
    CHECK(index.Buckets()[0].Total() == 1);
}

TEST_CASE("AddRow counts by level slot", "[histogram_bucket_index]")
{
    HistogramBucketIndex index{HistogramBucketSize::OneMinute};
    index.AddRow(Base(), LogLevel::Info);
    index.AddRow(Base() + 15s, LogLevel::Error);
    index.AddRow(Base() + 30s, LogLevel::Error);
    index.AddRow(Base() + 45s, LogLevel::Unknown);

    REQUIRE(index.Buckets().size() == 1);
    const auto &b = index.Buckets()[0];
    CHECK(b.counts[static_cast<size_t>(LogLevel::Info)] == 1);
    CHECK(b.counts[static_cast<size_t>(LogLevel::Error)] == 2);
    CHECK(b.counts[static_cast<size_t>(LogLevel::Unknown)] == 1);
    CHECK(b.Total() == 4);
    CHECK(index.TotalRowCount() == 4);
}

TEST_CASE("AddRow extends the vector for later buckets", "[histogram_bucket_index]")
{
    HistogramBucketIndex index{HistogramBucketSize::OneMinute};
    index.AddRow(Base(), LogLevel::Info);
    index.AddRow(Base() + std::chrono::minutes{3}, LogLevel::Warn);

    REQUIRE(index.Buckets().size() == 4);
    CHECK(index.Buckets()[0].Total() == 1);
    CHECK(index.Buckets()[1].Total() == 0);
    CHECK(index.Buckets()[2].Total() == 0);
    CHECK(index.Buckets()[3].counts[static_cast<size_t>(LogLevel::Warn)] == 1);
    CHECK(index.TotalRowCount() == 2);
}

TEST_CASE("AddRow shifts and rebases origin on out-of-order backfill", "[histogram_bucket_index]")
{
    HistogramBucketIndex index{HistogramBucketSize::OneMinute};
    index.AddRow(Base() + std::chrono::minutes{2}, LogLevel::Info);
    // Earlier row — must not lose its contribution.
    index.AddRow(Base(), LogLevel::Error);

    CHECK(index.Origin() == Base());
    REQUIRE(index.Buckets().size() == 3);
    CHECK(index.Buckets()[0].counts[static_cast<size_t>(LogLevel::Error)] == 1);
    CHECK(index.Buckets()[1].Total() == 0);
    CHECK(index.Buckets()[2].counts[static_cast<size_t>(LogLevel::Info)] == 1);
    CHECK(index.TotalRowCount() == 2);
}

TEST_CASE("BucketOf returns nullopt outside range", "[histogram_bucket_index]")
{
    HistogramBucketIndex index{HistogramBucketSize::OneMinute};
    CHECK(index.BucketOf(Base()) == std::nullopt);

    index.AddRow(Base() + std::chrono::minutes{1}, LogLevel::Info);
    // Origin is 00:01:00; single bucket [00:01:00, 00:02:00).
    CHECK(index.BucketOf(Base() + std::chrono::minutes{1}) == std::optional<size_t>{0});
    CHECK(index.BucketOf(Base() + std::chrono::minutes{1} + 30s) == std::optional<size_t>{0});
    CHECK(index.BucketOf(Base()) == std::nullopt);
    CHECK(index.BucketOf(Base() + std::chrono::minutes{2}) == std::nullopt);
}

TEST_CASE("Reset drops buckets but keeps bucket size", "[histogram_bucket_index]")
{
    HistogramBucketIndex index{HistogramBucketSize::TenSeconds};
    index.AddRow(Base(), LogLevel::Info);
    index.Reset();
    CHECK(index.Empty());
    CHECK(index.TotalRowCount() == 0);
    CHECK(index.BucketSize() == HistogramBucketSize::TenSeconds);
}

TEST_CASE("SetBucketSize resets on change and is a no-op on same rung", "[histogram_bucket_index]")
{
    HistogramBucketIndex index{HistogramBucketSize::OneMinute};
    index.AddRow(Base(), LogLevel::Info);

    // No-op on same size.
    index.SetBucketSize(HistogramBucketSize::OneMinute);
    CHECK(index.Buckets().size() == 1);
    CHECK(index.TotalRowCount() == 1);

    // Change resets and preserves the new rung.
    index.SetBucketSize(HistogramBucketSize::TenSeconds);
    CHECK(index.Empty());
    CHECK(index.BucketSize() == HistogramBucketSize::TenSeconds);
}

TEST_CASE("AutoBucketSize picks the coarsest rung under budget", "[histogram_bucket_index]")
{
    SECTION("Under 500 seconds -> OneSecond")
    {
        CHECK(HistogramBucketIndex::AutoBucketSize(Base(), Base() + 60s) == HistogramBucketSize::OneSecond);
    }
    SECTION("~1 hour of data -> TenSeconds or OneMinute depending on budget")
    {
        // 3600 s / 10 s = 360 buckets < 500 -> TenSeconds wins over OneMinute
        // because we walk the ladder from finest to coarsest.
        CHECK(
            HistogramBucketIndex::AutoBucketSize(Base(), Base() + std::chrono::hours{1})
            == HistogramBucketSize::TenSeconds
        );
    }
    SECTION("A full day -> OneMinute is the coarsest that fits 500")
    {
        // 86400 s / 60 s = 1440 buckets; too many.
        // 86400 s / 600 s = 144 buckets; fits.
        CHECK(
            HistogramBucketIndex::AutoBucketSize(Base(), Base() + std::chrono::hours{24})
            == HistogramBucketSize::TenMinutes
        );
    }
    SECTION("A month -> OneDay")
    {
        const auto month = std::chrono::hours{24 * 31};
        CHECK(HistogramBucketIndex::AutoBucketSize(Base(), Base() + month) == HistogramBucketSize::OneDay);
    }
    SECTION("Degenerate zero-width range -> OneSecond")
    {
        CHECK(HistogramBucketIndex::AutoBucketSize(Base(), Base()) == HistogramBucketSize::OneSecond);
    }
    SECTION("Swapped inputs are normalised")
    {
        CHECK(
            HistogramBucketIndex::AutoBucketSize(Base() + std::chrono::hours{1}, Base())
            == HistogramBucketSize::TenSeconds
        );
    }
}

TEST_CASE("TruncateToBucket floors correctly at day boundaries", "[histogram_bucket_index]")
{
    // A moment 2 h 34 min into the day truncates back to midnight for OneDay.
    const auto midnight = Base();
    const auto offset = std::chrono::hours{2} + std::chrono::minutes{34};
    CHECK(HistogramBucketIndex::TruncateToBucket(midnight + offset, HistogramBucketSize::OneDay) == midnight);
    CHECK(
        HistogramBucketIndex::TruncateToBucket(midnight + offset, HistogramBucketSize::OneHour)
        == midnight + std::chrono::hours{2}
    );
}

TEST_CASE("HistogramBucketIndex handles 100k dense rows quickly", "[histogram_bucket_index]")
{
    HistogramBucketIndex index{HistogramBucketSize::OneSecond};

    // 100,000 rows across 60 seconds. Deterministic level cycle.
    constexpr int ROWS = 100000;
    constexpr std::array<LogLevel, 6> LEVEL_CYCLE = {
        LogLevel::Info,
        LogLevel::Debug,
        LogLevel::Warn,
        LogLevel::Error,
        LogLevel::Trace,
        LogLevel::Info,
    };
    const auto step = std::chrono::microseconds{60'000'000 / ROWS};
    for (int i = 0; i < ROWS; ++i)
    {
        index.AddRow(Base() + step * i, LEVEL_CYCLE[i % LEVEL_CYCLE.size()]);
    }
    CHECK(index.TotalRowCount() == ROWS);
    // 60 buckets at 1s each covering the 60 s span (plus fencepost).
    CHECK(index.Buckets().size() >= 60);
    CHECK(index.Buckets().size() <= 61);
}
