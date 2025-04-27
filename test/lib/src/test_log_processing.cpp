#include "common.hpp"

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
    static const auto TZ_DATA = std::filesystem::path("tzdata");
    auto tzdataPath = std::filesystem::current_path() / TZ_DATA;
    if (!std::filesystem::exists(tzdataPath))
    {
        // For Visual studio generator which does not provide the complete path to binaries before build
        tzdataPath = std::filesystem::current_path().parent_path() / TZ_DATA;
    }

    Initialize(tzdataPath);
}

TEST_CASE("ParseTimestamps errors", "[log_processing]")
{
    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"key1", "value1"}}, LogFileReference(*logFile, 0));
    testLines.emplace_back(LogMap{{"key1", "value2"}}, LogFileReference(*logFile, 1));
    testLines.emplace_back(LogMap{{"key2", 42}}, LogFileReference(*logFile, 2));
    std::vector<std::string> testKeys = {"key1", "key2"};

    // Create LogData instance
    LogData logData(std::move(logFile), std::move(testLines), std::move(testKeys));

    // Create a configuration with no timestamp columns
    LogConfiguration configuration;
    LogConfiguration::Column column;
    column.header = "key1";
    column.keys = {"key1"};
    configuration.columns.push_back(column);
    column.header = "key2";
    column.keys = {"key2"};
    configuration.columns.push_back(column);

    // Parse timestamps
    auto errors = ParseTimestamps(logData, configuration);

    // Verify no errors were returned since no timestamp columns exist
    CHECK(errors.empty());

    configuration.columns[0].type = LogConfiguration::Type::Time;
    errors = ParseTimestamps(logData, configuration);

    // All lines failed to parse the timestamp
    CHECK(errors.size() == logData.Lines().size());
}

TEST_CASE("ParseTimestamps success for different formats", "[log_processing]")
{
    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"key", "2025-04-25T12:34:56+00:00"}}, LogFileReference(*logFile, 0));
    testLines.emplace_back(LogMap{{"key", "2025-04-25 12:34:56+00:00"}}, LogFileReference(*logFile, 1));
    testLines.emplace_back(LogMap{{"key", "2025-04-25T12:34:56"}}, LogFileReference(*logFile, 2));
    testLines.emplace_back(LogMap{{"key", "2025-04-25 12:34:56"}}, LogFileReference(*logFile, 3));
    std::vector<std::string> testKeys = {"key"};

    // Create LogData instance
    LogData logData(std::move(logFile), std::move(testLines), std::move(testKeys));

    // Create a configuration with no timestamp columns
    LogConfiguration configuration;
    LogConfiguration::Column column;
    column.header = "key";
    column.keys = {"key"};
    column.type = LogConfiguration::Type::Time;
    column.parseFormats = {"%FT%T%Ez", "%F %T%Ez", "%FT%T", "%F %T"};
    configuration.columns.push_back(column);

    // Parse timestamps
    auto errors = ParseTimestamps(logData, configuration);

    CHECK(errors.empty());
    REQUIRE(logData.Lines().size() == 4);

    const TimeStamp timestamp{std::chrono::microseconds{1745584496000000}};

    CHECK(std::get<TimeStamp>(logData.Lines()[0].GetValue("key")) == timestamp);
    CHECK(std::get<TimeStamp>(logData.Lines()[1].GetValue("key")) == timestamp);
    CHECK(std::get<TimeStamp>(logData.Lines()[2].GetValue("key")) == timestamp);
    CHECK(std::get<TimeStamp>(logData.Lines()[3].GetValue("key")) == timestamp);
}

TEST_CASE("TimeStampToLocalMillisecondsSinceEpoch", "[log_processing]")
{
    InitializeTimezoneData();

    // Create a known UTC timestamp (2023-01-01 00:00:00 UTC)
    auto utcMicroseconds = date::sys_days{date::year{2023} / 1 / 1}.time_since_epoch();
    TimeStamp timestamp = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>{
        std::chrono::duration_cast<std::chrono::microseconds>(utcMicroseconds)
    };

    // Convert to local milliseconds
    int64_t localMilliseconds = TimeStampToLocalMillisecondsSinceEpoch(timestamp);

    // Get the timezone offset for verification
    static auto tz = date::current_zone();
    auto info = tz->get_info(timestamp);
    int64_t expectedOffset = std::chrono::duration_cast<std::chrono::milliseconds>(info.offset).count();

    // Calculate expected milliseconds (UTC milliseconds + timezone offset)
    int64_t utcMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(utcMicroseconds).count();
    int64_t expectedMilliseconds = utcMilliseconds + expectedOffset;

    CHECK(localMilliseconds == expectedMilliseconds);
}

TEST_CASE("UtcMicrosecondsToLocalMilliseconds", "[log_processing]")
{
    InitializeTimezoneData();

    // Get the timezone for verification
    static auto tz = date::current_zone();

    // Define test cases as a vector of microsecond values
    std::vector<int64_t> testMicroseconds = {
        0,                   // Epoch start time
        -3600ll * 1000000ll, // One hour before epoch (1969-12-31 23:00:00 UTC)
        1672574400000000ll   // 2023-01-01 12:00:00 UTC (a positive timestamp)
    };

    for (const auto &microseconds : testMicroseconds)
    {
        // Calculate the expected result manually
        std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds> testTime{
            std::chrono::microseconds{microseconds}
        };
        auto info = tz->get_info(testTime);
        int64_t expectedOffset = std::chrono::duration_cast<std::chrono::milliseconds>(info.offset).count();
        int64_t utcMilliseconds = microseconds / 1000;
        int64_t expectedMilliseconds = utcMilliseconds + expectedOffset;

        // Test the function
        int64_t result = UtcMicrosecondsToLocalMilliseconds(microseconds);
        CHECK(result == expectedMilliseconds);
    }
}

TEST_CASE("LocalMillisecondsSinceEpochToTimeStamp", "[log_processing]")
{
    InitializeTimezoneData();

    // Create known test timestamps at different ranges
    std::vector<TimeStamp> testTimestamps = {
        // Recent time
        std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>{
            std::chrono::microseconds{1672574400000000} // 2023-01-01 12:00:00 UTC
        },
        // Epoch time
        std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>{std::chrono::microseconds{0}},
        // Pre-epoch time
        std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>{
            std::chrono::microseconds{-3600000000} // 1 hour before epoch
        }
    };

    for (const auto &originalTimestamp : testTimestamps)
    {
        // Convert to local milliseconds
        int64_t localMilliseconds = TimeStampToLocalMillisecondsSinceEpoch(originalTimestamp);

        // Convert back to TimeStamp
        TimeStamp convertedTimestamp = LocalMillisecondsSinceEpochToTimeStamp(localMilliseconds);

        // Calculate timestamps in milliseconds for comparison
        int64_t originalMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(originalTimestamp.time_since_epoch()).count();
        int64_t convertedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(convertedTimestamp.time_since_epoch()).count();

        // Verify the timestamps match (only compare millisecond precision since we're converting through milliseconds)
        CHECK(originalMs == convertedMs);
    }
}

TEST_CASE("UtcMicrosecondsToDateTimeString", "[log_processing]")
{
    InitializeTimezoneData();

    // Test with a specific known timestamp: 2023-05-15 10:30:45 UTC
    // 1684146645000000 microseconds since epoch
    int64_t testMicroseconds = 1684146645000000;

    // Get the expected format which should be "YYYY-MM-DD HH:MM:SS"
    std::string formattedDate = UtcMicrosecondsToDateTimeString(testMicroseconds);

    // The expected format is "%F %T" which translates to "YYYY-MM-DD HH:MM:SS"
    // We need to verify the structural format
    std::regex dateTimePattern(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}.000)");
    CHECK(std::regex_match(formattedDate, dateTimePattern));

    // Create the same timestamp manually to verify the actual values
    static auto tz = date::current_zone();
    std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds> utcTime{
        std::chrono::microseconds{testMicroseconds}
    };
    const date::zoned_time localTime{tz, std::chrono::round<std::chrono::milliseconds>(utcTime)};
    std::string expectedDate = date::format("%F %T", localTime);

    // Verify the actual formatted output matches our expectation
    CHECK(formattedDate == expectedDate);
}

TEST_CASE("TimeStampToDateTimeString", "[log_processing]")
{
    InitializeTimezoneData();

    // Test with extreme past date: 1900-01-01 00:00:00 UTC
    // -2208988800000000 microseconds (approximately 70 years before Unix epoch)
    TimeStamp pastDate{std::chrono::microseconds{-2208988800000000}};
    std::string pastFormatted = TimeStampToDateTimeString(pastDate);

    // Verify format with regex pattern
    std::regex dateTimePattern(R"(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}.000)");
    CHECK(std::regex_match(pastFormatted, dateTimePattern));

    // Test with extreme future date: 2100-01-01 00:00:00 UTC
    // 4102444800000000 microseconds (approximately 130 years after Unix epoch)
    TimeStamp futureDate{std::chrono::microseconds{4102444800000000}};
    std::string futureFormatted = TimeStampToDateTimeString(futureDate);

    // Verify format
    CHECK(std::regex_match(futureFormatted, dateTimePattern));

    // Manual verification of the timestamps
    static auto tz = date::current_zone();

    const date::zoned_time pastLocalTime{tz, std::chrono::round<std::chrono::milliseconds>(pastDate)};
    std::string expectedPastDate = date::format("%F %T", pastLocalTime);
    CHECK(pastFormatted == expectedPastDate);

    const date::zoned_time futureLocalTime{tz, std::chrono::round<std::chrono::milliseconds>(futureDate)};
    std::string expectedFutureDate = date::format("%F %T", futureLocalTime);
}
