#include "common.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_processing.hpp>

#include <catch2/catch_all.hpp>
#include <date/tz.h>

#include <chrono>
#include <regex>

using namespace loglib;

TEST_CASE("Initialize function should throw an exception when an invalid path is provided", "[log_processing]")
{
    const auto invalidPath = std::filesystem::path("non_existent_path");
    CHECK_THROWS_AS(Initialize(invalidPath), std::runtime_error);
}

TEST_CASE("Initialize function should correctly set up timezone database with a valid path", "[log_processing]")
{
    InitializeTimezoneData();
}

TEST_CASE("ParseTimestamps errors", "[log_processing]")
{
    const TestLogFile testLogFile;
    auto source = testLogFile.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();
    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"key1", std::string("value1")}}, testKeys, *sourcePtr, 0);
    testLines.emplace_back(LogMap{{"key1", std::string("value2")}}, testKeys, *sourcePtr, 1);
    testLines.emplace_back(LogMap{{"key2", int64_t{42}}}, testKeys, *sourcePtr, 2);

    LogData logData(std::move(source), std::move(testLines), std::move(testKeys));

    // Configuration with two non-time columns initially.
    LogConfiguration configuration;
    LogConfiguration::Column column;
    column.header = "key1";
    column.keys = {"key1"};
    configuration.columns.push_back(column);
    column.header = "key2";
    column.keys = {"key2"};
    configuration.columns.push_back(column);

    auto errors = ParseTimestamps(logData, configuration);
    CHECK(errors.empty());

    configuration.columns[0].type = LogConfiguration::Type::time;
    errors = ParseTimestamps(logData, configuration);

    CHECK(errors.size() == logData.Lines().size());
}

TEST_CASE("ParseTimestamps success for different formats", "[log_processing]")
{
    const TestLogFile testLogFile;
    auto source = testLogFile.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();
    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"key", std::string("2025-04-25T12:34:56+00:00")}}, testKeys, *sourcePtr, 0);
    testLines.emplace_back(LogMap{{"key", std::string("2025-04-25 12:34:56+00:00")}}, testKeys, *sourcePtr, 1);
    testLines.emplace_back(LogMap{{"key", std::string("2025-04-25T12:34:56")}}, testKeys, *sourcePtr, 2);
    testLines.emplace_back(LogMap{{"key", std::string("2025-04-25 12:34:56")}}, testKeys, *sourcePtr, 3);

    LogData logData(std::move(source), std::move(testLines), std::move(testKeys));

    // Configuration with one Type::time column.
    LogConfiguration configuration;
    LogConfiguration::Column column;
    column.header = "key";
    column.keys = {"key"};
    column.type = LogConfiguration::Type::time;
    column.parseFormats = {"%FT%T%Ez", "%F %T%Ez", "%FT%T", "%F %T"};
    configuration.columns.push_back(column);

    auto errors = ParseTimestamps(logData, configuration);

    CHECK(errors.empty());
    REQUIRE(logData.Lines().size() == 4);

    const TimeStamp timestamp{std::chrono::microseconds{1745584496000000}};

    CHECK(std::get<TimeStamp>(logData.Lines()[0].GetValue("key")) == timestamp);
    CHECK(std::get<TimeStamp>(logData.Lines()[1].GetValue("key")) == timestamp);
    CHECK(std::get<TimeStamp>(logData.Lines()[2].GetValue("key")) == timestamp);
    CHECK(std::get<TimeStamp>(logData.Lines()[3].GetValue("key")) == timestamp);
}

TEST_CASE("ClassifyTimestampFormat", "[log_processing][iso8601_fast_path]")
{
    CHECK(ClassifyTimestampFormat("%FT%T") == TimestampFormatKind::Iso8601_T);
    CHECK(ClassifyTimestampFormat("%F %T") == TimestampFormatKind::Iso8601_Space);
    CHECK(ClassifyTimestampFormat("%FT%T%Ez") == TimestampFormatKind::Generic);
    CHECK(ClassifyTimestampFormat("%F %T%Ez") == TimestampFormatKind::Generic);
    CHECK(ClassifyTimestampFormat("") == TimestampFormatKind::Generic);
    CHECK(ClassifyTimestampFormat(" %FT%T") == TimestampFormatKind::Generic);
    CHECK(ClassifyTimestampFormat("%FT%T ") == TimestampFormatKind::Generic);
}

TEST_CASE("TryParseIsoTimestamp accepts valid inputs", "[log_processing][iso8601_fast_path]")
{
    const TimeStamp expectedNoFraction{std::chrono::microseconds{1745584496000000}};
    const TimeStamp expectedMs{std::chrono::microseconds{1745584496123000}};
    const TimeStamp expectedUs{std::chrono::microseconds{1745584496123456}};

    SECTION("%FT%T no fractional seconds")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("2025-04-25T12:34:56", 'T', out));
        CHECK(out == expectedNoFraction);
    }

    SECTION("%FT%T with millisecond fraction")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("2025-04-25T12:34:56.123", 'T', out));
        CHECK(out == expectedMs);
    }

    SECTION("%FT%T with microsecond fraction")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("2025-04-25T12:34:56.123456", 'T', out));
        CHECK(out == expectedUs);
    }

    SECTION("%F %T no fractional seconds")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("2025-04-25 12:34:56", ' ', out));
        CHECK(out == expectedNoFraction);
    }

    SECTION("%F %T with millisecond fraction")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("2025-04-25 12:34:56.123", ' ', out));
        CHECK(out == expectedMs);
    }

    SECTION("%F %T with microsecond fraction")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("2025-04-25 12:34:56.123456", ' ', out));
        CHECK(out == expectedUs);
    }

    SECTION("Single-digit fractional pads to microseconds")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("2025-04-25T12:34:56.5", 'T', out));
        CHECK(out == TimeStamp{std::chrono::microseconds{1745584496500000}});
    }

    SECTION("Two-digit fractional pads to microseconds")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("2025-04-25T12:34:56.50", 'T', out));
        CHECK(out == TimeStamp{std::chrono::microseconds{1745584496500000}});
    }

    // Regression for #12: epoch and pre-1970 timestamps are valid outputs.
    SECTION("Posix epoch is parsed (not reported as failure)")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("1970-01-01T00:00:00", 'T', out));
        CHECK(out == TimeStamp{std::chrono::microseconds{0}});
    }

    SECTION("Pre-epoch timestamp is parsed (not reported as failure)")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("1969-12-31T23:00:00", 'T', out));
        CHECK(out == TimeStamp{std::chrono::microseconds{-3600000000LL}});
    }
}

TEST_CASE("TryParseIsoTimestamp rejects malformed inputs", "[log_processing][iso8601_fast_path]")
{
    TimeStamp out{};

    SECTION("Empty string")
    {
        CHECK_FALSE(TryParseIsoTimestamp("", 'T', out));
    }

    SECTION("Too short")
    {
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34", 'T', out));
    }

    SECTION("Wrong date/time separator")
    {
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25 12:34:56", 'T', out));
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:56", ' ', out));
    }

    SECTION("Invalid month")
    {
        CHECK_FALSE(TryParseIsoTimestamp("2025-13-01T12:34:56", 'T', out));
        CHECK_FALSE(TryParseIsoTimestamp("2025-00-01T12:34:56", 'T', out));
    }

    SECTION("Invalid day-of-month")
    {
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-31T12:34:56", 'T', out)); // April has 30 days
        CHECK_FALSE(TryParseIsoTimestamp("2025-02-29T12:34:56", 'T', out)); // 2025 is not leap
    }

    SECTION("Invalid hour")
    {
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T24:00:00", 'T', out));
    }

    SECTION("Invalid minute")
    {
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:60:00", 'T', out));
    }

    SECTION("Non-digit in fixed positions")
    {
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:5x", 'T', out));
        CHECK_FALSE(TryParseIsoTimestamp("20a5-04-25T12:34:56", 'T', out));
    }

    SECTION("Trailing garbage after seconds")
    {
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:56Z", 'T', out));
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:56+00:00", 'T', out));
    }

    SECTION("Empty fractional after dot")
    {
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:56.", 'T', out));
    }

    SECTION("Sub-microsecond fractional precision")
    {
        // 7+ digits must fall through to the slow path; no precision loss.
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:56.1234567", 'T', out));
    }

    SECTION("Trailing garbage after fractional digits")
    {
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:56.123Z", 'T', out));
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:56.123x", 'T', out));
    }
}

TEST_CASE("TryParseIsoTimestamp matches date::parse for representative inputs", "[log_processing][iso8601_fast_path]")
{
    const std::vector<std::string> isoTInputs = {
        "1970-01-01T00:00:01",
        "2000-02-29T23:59:59", // leap year
        "2024-12-31T23:59:59.999",
        "2025-04-25T12:34:56",
        "2025-04-25T12:34:56.123",
        "2025-04-25T12:34:56.123456",
        "2099-06-15T03:04:05.000001",
    };

    for (const auto &input : isoTInputs)
    {
        CAPTURE(input);
        TimeStamp fastOut{};
        REQUIRE(TryParseIsoTimestamp(input, 'T', fastOut));

        TimestampParseScratch scratch;
        TimeStamp slowOut{};
        REQUIRE(TryParseGenericTimestamp(input, "%FT%T", scratch, slowOut));

        CHECK(fastOut == slowOut);
    }
}

TEST_CASE("TryParseTimestamp dispatches on kind", "[log_processing][iso8601_fast_path]")
{
    TimestampParseScratch scratch;
    TimeStamp out{};

    SECTION("Iso8601_T routes to fast path")
    {
        REQUIRE(TryParseTimestamp("2025-04-25T12:34:56", "%FT%T", TimestampFormatKind::Iso8601_T, scratch, out));
        CHECK(out == TimeStamp{std::chrono::microseconds{1745584496000000}});
    }

    SECTION("Iso8601_Space routes to fast path")
    {
        REQUIRE(TryParseTimestamp("2025-04-25 12:34:56", "%F %T", TimestampFormatKind::Iso8601_Space, scratch, out));
        CHECK(out == TimeStamp{std::chrono::microseconds{1745584496000000}});
    }

    SECTION("Generic routes to date::parse")
    {
        REQUIRE(TryParseTimestamp("2025-04-25T12:34:56+00:00", "%FT%T%Ez", TimestampFormatKind::Generic, scratch, out));
        CHECK(out == TimeStamp{std::chrono::microseconds{1745584496000000}});
    }

    SECTION("Generic rejects malformed input")
    {
        CHECK_FALSE(TryParseTimestamp("not a date", "%FT%T%Ez", TimestampFormatKind::Generic, scratch, out));
    }
}

TEST_CASE("TimeStampToLocalMillisecondsSinceEpoch", "[log_processing]")
{
    InitializeTimezoneData();

    // 2023-01-01 00:00:00 UTC.
    auto utcMicroseconds = date::sys_days{date::year{2023} / 1 / 1}.time_since_epoch();
    const TimeStamp timestamp = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>{
        std::chrono::duration_cast<std::chrono::microseconds>(utcMicroseconds)
    };

    const int64_t localMilliseconds = TimeStampToLocalMillisecondsSinceEpoch(timestamp);

    static const auto *tz = date::current_zone();
    auto info = tz->get_info(timestamp);
    const int64_t expectedOffset = std::chrono::duration_cast<std::chrono::milliseconds>(info.offset).count();

    const int64_t utcMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(utcMicroseconds).count();
    const int64_t expectedMilliseconds = utcMilliseconds + expectedOffset;

    CHECK(localMilliseconds == expectedMilliseconds);
}

TEST_CASE("UtcMicrosecondsToLocalMilliseconds", "[log_processing]")
{
    InitializeTimezoneData();

    static const auto *tz = date::current_zone();

    const std::vector<int64_t> testMicroseconds = {
        0,                   // Epoch start time
        -3600LL * 1000000LL, // 1969-12-31 23:00:00 UTC
        1672574400000000LL   // 2023-01-01 12:00:00 UTC
    };

    for (const auto &microseconds : testMicroseconds)
    {
        const std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds> testTime{
            std::chrono::microseconds{microseconds}
        };
        auto info = tz->get_info(testTime);
        const int64_t expectedOffset = std::chrono::duration_cast<std::chrono::milliseconds>(info.offset).count();
        const int64_t utcMilliseconds = microseconds / 1000;
        const int64_t expectedMilliseconds = utcMilliseconds + expectedOffset;

        const int64_t result = UtcMicrosecondsToLocalMilliseconds(microseconds);
        CHECK(result == expectedMilliseconds);
    }
}

TEST_CASE("LocalMillisecondsSinceEpochToTimeStamp", "[log_processing]")
{
    InitializeTimezoneData();

    const std::vector<TimeStamp> testTimestamps = {
        std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>{
            std::chrono::microseconds{1672574400000000} // 2023-01-01 12:00:00 UTC
        },
        std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>{std::chrono::microseconds{0}},
        std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>{
            std::chrono::microseconds{-3600000000} // 1 hour before epoch
        }
    };

    for (const auto &originalTimestamp : testTimestamps)
    {
        const int64_t localMilliseconds = TimeStampToLocalMillisecondsSinceEpoch(originalTimestamp);
        const TimeStamp convertedTimestamp = LocalMillisecondsSinceEpochToTimeStamp(localMilliseconds);

        const int64_t originalMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(originalTimestamp.time_since_epoch()).count();
        const int64_t convertedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(convertedTimestamp.time_since_epoch()).count();

        // Round-trip is millisecond-precision (the int64_t carrier).
        CHECK(originalMs == convertedMs);
    }
}

TEST_CASE("UtcMicrosecondsToDateTimeString", "[log_processing]")
{
    InitializeTimezoneData();

    // 2023-05-15 10:30:45 UTC.
    const int64_t testMicroseconds = 1684146645000000;

    std::string formattedDate = UtcMicrosecondsToDateTimeString(testMicroseconds);

    const std::regex dateTimePattern(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}.000)");
    CHECK(std::regex_match(formattedDate, dateTimePattern));

    static const auto *tz = date::current_zone();
    const std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds> utcTime{
        std::chrono::microseconds{testMicroseconds}
    };
    const date::zoned_time localTime{tz, std::chrono::round<std::chrono::milliseconds>(utcTime)};
    std::string expectedDate = date::format("%F %T", localTime);

    CHECK(formattedDate == expectedDate);
}

TEST_CASE("TimeStampToDateTimeString", "[log_processing]")
{
    InitializeTimezoneData();

    // 1900-01-01 00:00:00 UTC (~70y before Unix epoch).
    const TimeStamp pastDate{std::chrono::microseconds{-2208988800000000}};
    std::string pastFormatted = TimeStampToDateTimeString(pastDate);

    const std::regex dateTimePattern(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}.000)");
    CHECK(std::regex_match(pastFormatted, dateTimePattern));

    // 2100-01-01 00:00:00 UTC (~130y after Unix epoch).
    const TimeStamp futureDate{std::chrono::microseconds{4102444800000000}};
    const std::string futureFormatted = TimeStampToDateTimeString(futureDate);

    CHECK(std::regex_match(futureFormatted, dateTimePattern));

    static const auto *tz = date::current_zone();

    const date::zoned_time pastLocalTime{tz, std::chrono::round<std::chrono::milliseconds>(pastDate)};
    std::string expectedPastDate = date::format("%F %T", pastLocalTime);
    CHECK(pastFormatted == expectedPastDate);

    const date::zoned_time futureLocalTime{tz, std::chrono::round<std::chrono::milliseconds>(futureDate)};
    const std::string expectedFutureDate = date::format("%F %T", futureLocalTime);
    CHECK(futureFormatted == expectedFutureDate);
}
