#include "common.hpp"

#include <loglib/core/key_index.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/core/log_value.hpp>
#include <loglib/time/internal/timestamp_promotion.hpp>
#include <loglib/time/time_zone_context.hpp>
#include <loglib/time/timestamp_parsing.hpp>

#include <catch2/catch_all.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace loglib;
using loglib::internal::LastTimestampBytesHit;
using loglib::internal::PromoteLineTimestamps;
using loglib::internal::TimeColumnSpec;

namespace
{

constexpr std::int64_t MICROS_PER_SECOND = 1'000'000;

/** @brief Microseconds since the epoch of a wall-clock time read as UTC. */
std::int64_t Micros(int year, unsigned month, unsigned day, int hour, int minute, int second)
{
    const auto days =
        std::chrono::sys_days{std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}};
    const auto total = std::chrono::duration_cast<std::chrono::microseconds>(days.time_since_epoch()) +
                       std::chrono::hours{hour} + std::chrono::minutes{minute} + std::chrono::seconds{second};
    return total.count();
}

/** @brief A time column over the given keys and formats, read in UTC. */
TimeColumnSpec MakeSpec(KeyIndex &keys, const std::vector<std::string> &keyNames, std::vector<std::string> formats)
{
    TimeColumnSpec spec;
    for (const std::string &name : keyNames)
    {
        spec.keyIds.push_back(keys.GetOrInsert(name));
    }
    spec.parseFormats = std::move(formats);
    for (const std::string &format : spec.parseFormats)
    {
        spec.formatKinds.push_back(ClassifyTimestampFormat(format));
    }
    spec.timeZone = TimeZoneContext::Utc();
    spec.referenceTime =
        std::chrono::system_clock::time_point{std::chrono::seconds{Micros(2024, 6, 1, 0, 0, 0) / MICROS_PER_SECOND}};
    return spec;
}

struct PromotionState
{
    std::vector<std::optional<LastValidTimestampParse>> lastValid;
    std::vector<LastTimestampBytesHit> bytesHits;
    TimestampParseScratch scratch;

    explicit PromotionState(std::size_t columns)
        : lastValid(columns), bytesHits(columns)
    {
    }
};

std::optional<std::int64_t> TimeOf(const LogLine &line, const KeyIndex &keys, std::string_view key)
{
    const LogValue value = line.GetValue(keys, key);
    if (const auto *stamp = std::get_if<TimeStamp>(&value))
    {
        return stamp->time_since_epoch().count();
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("PromoteLineTimestamps replaces the text of a time column with a timestamp", "[timestamp_promotion]")
{
    auto source = MemorySource("line\n");
    KeyIndex keys;
    const std::array<TimeColumnSpec, 1> specs{MakeSpec(keys, {"time"}, {"%F %T"})};
    PromotionState state(1);

    LogLine line(LogMap{{"time", std::string("2024-05-01 12:30:45")}, {"msg", std::string("hi")}}, keys, *source, 0);
    CHECK(PromoteLineTimestamps(line, specs, state.lastValid, state.bytesHits, state.scratch, {}));

    CHECK(TimeOf(line, keys, "time") == Micros(2024, 5, 1, 12, 30, 45));
    CHECK(std::get<std::string>(line.GetValue(keys, "msg")) == "hi");
    REQUIRE(state.lastValid[0].has_value());
    CHECK(state.lastValid[0]->format == "%F %T");
}

TEST_CASE("PromoteLineTimestamps leaves a line without a matching key or format alone", "[timestamp_promotion]")
{
    auto source = MemorySource("line\n");
    KeyIndex keys;
    const std::array<TimeColumnSpec, 1> specs{MakeSpec(keys, {"time"}, {"%F %T"})};
    PromotionState state(1);

    SECTION("no key")
    {
        LogLine line(LogMap{{"other", std::string("2024-05-01 12:30:45")}}, keys, *source, 0);
        CHECK_FALSE(PromoteLineTimestamps(line, specs, state.lastValid, state.bytesHits, state.scratch, {}));
        CHECK(std::holds_alternative<std::string>(line.GetValue(keys, "other")));
    }
    SECTION("text that no format reads")
    {
        LogLine line(LogMap{{"time", std::string("yesterday")}}, keys, *source, 0);
        CHECK_FALSE(PromoteLineTimestamps(line, specs, state.lastValid, state.bytesHits, state.scratch, {}));
        CHECK(std::get<std::string>(line.GetValue(keys, "time")) == "yesterday");
        CHECK_FALSE(state.lastValid[0].has_value());
    }
    SECTION("a value that is not text")
    {
        LogLine line(LogMap{{"time", static_cast<int64_t>(5)}}, keys, *source, 0);
        CHECK_FALSE(PromoteLineTimestamps(line, specs, state.lastValid, state.bytesHits, state.scratch, {}));
        CHECK(std::get<int64_t>(line.GetValue(keys, "time")) == 5);
    }
}

TEST_CASE("PromoteLineTimestamps tries the keys and the formats in order", "[timestamp_promotion]")
{
    auto source = MemorySource("line\n");
    KeyIndex keys;
    const std::array<TimeColumnSpec, 1> specs{MakeSpec(keys, {"ts", "time"}, {"%F %T", "%d/%m/%Y %T"})};
    PromotionState state(1);

    SECTION("the first key that is present wins")
    {
        LogLine line(
            LogMap{{"ts", std::string("2024-05-01 01:00:00")}, {"time", std::string("2024-05-02 02:00:00")}},
            keys,
            *source,
            0
        );
        REQUIRE(PromoteLineTimestamps(line, specs, state.lastValid, state.bytesHits, state.scratch, {}));
        CHECK(TimeOf(line, keys, "ts") == Micros(2024, 5, 1, 1, 0, 0));
        CHECK(std::holds_alternative<std::string>(line.GetValue(keys, "time")));
    }
    SECTION("the second key is used when the first is missing")
    {
        LogLine line(LogMap{{"time", std::string("2024-05-02 02:00:00")}}, keys, *source, 0);
        REQUIRE(PromoteLineTimestamps(line, specs, state.lastValid, state.bytesHits, state.scratch, {}));
        CHECK(TimeOf(line, keys, "time") == Micros(2024, 5, 2, 2, 0, 0));
    }
    SECTION("the second format is used when the first does not read the text")
    {
        LogLine line(LogMap{{"ts", std::string("03/05/2024 03:00:00")}}, keys, *source, 0);
        REQUIRE(PromoteLineTimestamps(line, specs, state.lastValid, state.bytesHits, state.scratch, {}));
        CHECK(TimeOf(line, keys, "ts") == Micros(2024, 5, 3, 3, 0, 0));
        REQUIRE(state.lastValid[0].has_value());
        CHECK(state.lastValid[0]->format == "%d/%m/%Y %T");
    }
}

TEST_CASE("PromoteLineTimestamps reuses the last format and the last text", "[timestamp_promotion]")
{
    auto source = MemorySource("line\n");
    KeyIndex keys;
    const std::array<TimeColumnSpec, 1> specs{MakeSpec(keys, {"time"}, {"%d/%m/%Y %T", "%F %T"})};
    PromotionState state(1);

    LogLine first(LogMap{{"time", std::string("2024-05-01 12:00:00")}}, keys, *source, 0);
    REQUIRE(PromoteLineTimestamps(first, specs, state.lastValid, state.bytesHits, state.scratch, {}));
    REQUIRE(state.lastValid[0].has_value());
    CHECK(state.lastValid[0]->format == "%F %T");
    CHECK(state.bytesHits[0].valid);
    CHECK(state.bytesHits[0].bytes == "2024-05-01 12:00:00");

    // The same text on the next line gives the same value from the remembered hit.
    LogLine same(LogMap{{"time", std::string("2024-05-01 12:00:00")}}, keys, *source, 1);
    REQUIRE(PromoteLineTimestamps(same, specs, state.lastValid, state.bytesHits, state.scratch, {}));
    CHECK(TimeOf(same, keys, "time") == Micros(2024, 5, 1, 12, 0, 0));

    // Different text is parsed again with the remembered format.
    LogLine next(LogMap{{"time", std::string("2024-05-01 12:00:01")}}, keys, *source, 2);
    REQUIRE(PromoteLineTimestamps(next, specs, state.lastValid, state.bytesHits, state.scratch, {}));
    CHECK(TimeOf(next, keys, "time") == Micros(2024, 5, 1, 12, 0, 1));
    CHECK(state.bytesHits[0].bytes == "2024-05-01 12:00:01");
}

TEST_CASE("PromoteLineTimestamps converts every time column and reports any success", "[timestamp_promotion]")
{
    auto source = MemorySource("line\n");
    KeyIndex keys;
    const std::array<TimeColumnSpec, 2> specs{MakeSpec(keys, {"a"}, {"%F %T"}), MakeSpec(keys, {"b"}, {"%F %T"})};
    PromotionState state(2);

    LogLine line(LogMap{{"a", std::string("2024-05-01 01:00:00")}, {"b", std::string("nonsense")}}, keys, *source, 0);
    CHECK(PromoteLineTimestamps(line, specs, state.lastValid, state.bytesHits, state.scratch, {}));
    CHECK(TimeOf(line, keys, "a") == Micros(2024, 5, 1, 1, 0, 0));
    CHECK(std::get<std::string>(line.GetValue(keys, "b")) == "nonsense");
    CHECK(state.lastValid[0].has_value());
    CHECK_FALSE(state.lastValid[1].has_value());
}

TEST_CASE("PromoteLineTimestamps reads a time without a zone in the column zone", "[timestamp_promotion]")
{
    InitializeTimezoneData();
    auto source = MemorySource("line\n");
    KeyIndex keys;
    std::array<TimeColumnSpec, 1> specs{MakeSpec(keys, {"time"}, {"%F %T"})};
    specs[0].timeZone = TimeZoneContext::Load(FindTestTzdata(), "Europe/Berlin");
    PromotionState state(1);

    // Berlin is UTC+2 in May.
    LogLine summer(LogMap{{"time", std::string("2024-05-01 12:00:00")}}, keys, *source, 0);
    REQUIRE(PromoteLineTimestamps(summer, specs, state.lastValid, state.bytesHits, state.scratch, {}));
    CHECK(TimeOf(summer, keys, "time") == Micros(2024, 5, 1, 10, 0, 0));

    // Berlin is UTC+1 in January.
    LogLine winter(LogMap{{"time", std::string("2024-01-15 12:00:00")}}, keys, *source, 1);
    REQUIRE(PromoteLineTimestamps(winter, specs, state.lastValid, state.bytesHits, state.scratch, {}));
    CHECK(TimeOf(winter, keys, "time") == Micros(2024, 1, 15, 11, 0, 0));
}

TEST_CASE("PromoteLineTimestamps keeps an explicit offset and ignores the column zone", "[timestamp_promotion]")
{
    InitializeTimezoneData();
    auto source = MemorySource("line\n");
    KeyIndex keys;
    std::array<TimeColumnSpec, 1> specs{MakeSpec(keys, {"time"}, {"%FT%T%Ez"})};
    specs[0].timeZone = TimeZoneContext::Load(FindTestTzdata(), "America/New_York");
    PromotionState state(1);

    LogLine line(LogMap{{"time", std::string("2024-05-01T12:00:00+02:00")}}, keys, *source, 0);
    REQUIRE(PromoteLineTimestamps(line, specs, state.lastValid, state.bytesHits, state.scratch, {}));
    CHECK(TimeOf(line, keys, "time") == Micros(2024, 5, 1, 10, 0, 0));
}
