#include "common.hpp"

#include <loglib/core/file_line_source.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/query/query_parser.hpp>
#include <loglib/schema/column_schema.hpp>
#include <loglib/schema/filter_expression.hpp>
#include <loglib/table/internal/timestamp_backfill.hpp>
#include <loglib/time/time_zone_context.hpp>
#include <loglib/time/timestamp_parsing.hpp>
#include <loglib/time/user_timestamp.hpp>

#include <catch2/catch_all.hpp>
#include <date/tz.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace loglib;

namespace
{
/** @brief A fixed "now" for formats without a year, so the assumed year does not depend on the day the test runs. */
const std::chrono::system_clock::time_point REFERENCE_NOW{
    std::chrono::sys_days{std::chrono::year{2025} / std::chrono::June / 15} + std::chrono::hours{12}
};
} // namespace

TEST_CASE("TimeZoneContext::Load throws when the tzdata path is invalid", "[log_processing]")
{
    const auto invalidPath = std::filesystem::path("non_existent_path");
    CHECK_THROWS_AS(TimeZoneContext::Load(invalidPath), std::runtime_error);
}

TEST_CASE("TimeZoneContext::Load accepts the staged tzdata directory", "[log_processing]")
{
    InitializeTimezoneData();
    CHECK_FALSE(ProcessDefaultTimeZone().IanaName().empty());
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
    Column column;
    column.header = "key1";
    column.keys = {"key1"};
    configuration.columns.push_back(column);
    column.header = "key2";
    column.keys = {"key2"};
    configuration.columns.push_back(column);

    auto errors = ParseTimestamps(logData, configuration, TimeZoneContext{}, REFERENCE_NOW);
    CHECK(errors.empty());

    configuration.columns[0].type = ColumnType::Time;
    errors = ParseTimestamps(logData, configuration, TimeZoneContext{}, REFERENCE_NOW);

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

    // Configuration with one Type::Time column.
    LogConfiguration configuration;
    Column column;
    column.header = "key";
    column.keys = {"key"};
    column.type = ColumnType::Time;
    column.parseFormats = {"%FT%T%Ez", "%F %T%Ez", "%FT%T", "%F %T"};
    configuration.columns.push_back(column);

    auto errors = ParseTimestamps(logData, configuration, TimeZoneContext{}, REFERENCE_NOW);

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
    CHECK(ClassifyTimestampFormat("%FT%T") == TimestampFormatKind::Iso8601T);
    CHECK(ClassifyTimestampFormat("%F %T") == TimestampFormatKind::Iso8601Space);
    CHECK(ClassifyTimestampFormat("%FT%T%Ez") == TimestampFormatKind::Iso8601TOffset);
    CHECK(ClassifyTimestampFormat("%F %T%Ez") == TimestampFormatKind::Iso8601SpaceOffset);
    CHECK(ClassifyTimestampFormat("") == TimestampFormatKind::Generic);
    CHECK(ClassifyTimestampFormat(" %FT%T") == TimestampFormatKind::Generic);
    CHECK(ClassifyTimestampFormat("%FT%T ") == TimestampFormatKind::Generic);
}

TEST_CASE(
    "Default time formats keep the instant across fraction widths and zones", "[log_processing][timestamp_corpus]"
)
{
    const auto parse = [](std::string_view text) -> std::optional<TimeStamp> {
        TimestampParseScratch scratch;
        for (const std::string &format : DefaultTimeParseFormats())
        {
            TimeStamp out{};
            if (TryParseTimestamp(text, format, ClassifyTimestampFormat(format), scratch, REFERENCE_NOW, out))
            {
                return out;
            }
        }
        return std::nullopt;
    };
    const auto at = [](std::int64_t seconds, std::int64_t fraction) {
        return std::optional<TimeStamp>{TimeStamp{std::chrono::seconds{seconds} + std::chrono::microseconds{fraction}}};
    };
    constexpr std::int64_t BASE = 1704103200; // 2024-01-01T10:00:00Z

    CHECK(parse("2024-01-01T10:00:00Z") == at(BASE, 0));
    CHECK(parse("2024-01-01T10:00:00.123456Z") == at(BASE, 123456));
    CHECK(parse("2024-01-01T10:00:00.1234567Z") == at(BASE, 123456));
    CHECK(parse("2024-01-01T10:00:00.000000123Z") == at(BASE, 0));
    CHECK(parse("2024-01-01T10:00:00.123456789Z") == at(BASE, 123456));
    CHECK(parse("2024-01-01T10:00:00.999999999Z") == at(BASE, 999999));
    CHECK(parse("2024-01-01 10:00:00.123456789") == at(BASE, 123456));
    CHECK(parse("2024-01-01T10:00:00.12345678") == at(BASE, 123456));
    CHECK(parse("2024-01-01T11:00:00+01:00") == at(BASE, 0));
    CHECK(parse("2024-01-01T11:00:00.123456789+01:00") == at(BASE, 123456));
    CHECK(parse("2024-01-01T04:30:00-05:30") == at(BASE, 0));
    CHECK(parse("2024-01-01T11:00:00+0100") == at(BASE, 0));
    CHECK(parse("2024-01-01T11:00:00+01") == at(BASE, 0));
    CHECK(parse("2024-01-01 11:00:00+01:00") == at(BASE, 0));
    CHECK(parse("01/Jan/2024:10:00:00 +0000") == at(BASE, 0));

    CHECK_FALSE(parse("2024-01-01T10:00:00 garbage").has_value());
    CHECK_FALSE(parse("2024-01-01T10:00:00+01:00 garbage").has_value());
    CHECK_FALSE(parse("2024-01-01T10:00:00+25:00").has_value());
    CHECK_FALSE(parse("2024-01-01T10:00:00+01:").has_value());
    CHECK_FALSE(parse("01/Jan/2024:10:00:00 +0000 garbage").has_value());
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

    // Session bundles emit UTC timestamps with a trailing `Z`.
    SECTION("Trailing Z after seconds")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("2025-04-25T12:34:56Z", 'T', out));
        CHECK(out == expectedNoFraction);
    }

    SECTION("Trailing Z after millisecond fraction")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("2025-04-25T12:34:56.123Z", 'T', out));
        CHECK(out == expectedMs);
    }

    SECTION("Trailing Z after microsecond fraction")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("2025-04-25T12:34:56.123456Z", 'T', out));
        CHECK(out == expectedUs);
    }

    SECTION("Trailing Z with space date/time separator")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("2025-04-25 12:34:56.123Z", ' ', out));
        CHECK(out == expectedMs);
    }

    // ISO 8601 §4.2.2.4 permits `,` as the fractional separator;
    // Java Logback / log4j2 / SLF4J's default PatternLayout
    // (`2024-04-28 04:02:03,123 INFO ...`) is the most visible
    // downstream. The fast path handles both spellings so the
    // shared `%FT%T` / `%F %T` parseFormat entries cover both
    // without a dedicated Java-only format string.
    SECTION("Comma fractional separator with space date/time separator")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("2025-04-25 12:34:56,123", ' ', out));
        CHECK(out == expectedMs);
    }

    SECTION("Comma fractional separator with T date/time separator")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("2025-04-25T12:34:56,123", 'T', out));
        CHECK(out == expectedMs);
    }

    SECTION("Comma fractional separator at microsecond precision")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("2025-04-25T12:34:56,123456", 'T', out));
        CHECK(out == expectedUs);
    }

    SECTION("Trailing Z after comma fractional")
    {
        TimeStamp out{};
        REQUIRE(TryParseIsoTimestamp("2025-04-25T12:34:56,123Z", 'T', out));
        CHECK(out == expectedMs);
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
        // Numeric offsets remain on the generic parser path.
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:56+00:00", 'T', out));
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:56ZZ", 'T', out));
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:56Zx", 'T', out));
    }

    SECTION("Empty fractional after dot")
    {
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:56.", 'T', out));
        // `.` followed only by `Z` is not a valid ISO-8601 fraction.
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:56.Z", 'T', out));
    }

    SECTION("Empty fractional after comma")
    {
        // Same rules apply to the ISO-8601 `,` separator as to `.`.
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:56,", 'T', out));
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:56,Z", 'T', out));
    }

    SECTION("Trailing garbage after fractional digits")
    {
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:56.123x", 'T', out));
        CHECK_FALSE(TryParseIsoTimestamp("2025-04-25T12:34:56.123Zx", 'T', out));
    }
}

TEST_CASE("TryParseSyslogRfc3164Timestamp accepts valid inputs", "[log_processing][syslog_rfc3164_fast_path]")
{
    // The parser injects the year via the "if parsed month is later
    // than the reference month, use the previous year" heuristic. A fixed
    // reference time makes the year the same on every day the test runs.
    const auto today = date::floor<date::days>(REFERENCE_NOW);
    const date::year_month_day nowYmd{today};
    const unsigned nowMonth = static_cast<unsigned>(nowYmd.month());

    auto expected = [&](unsigned month, int day, int hour, int minute, int second) {
        int year = static_cast<int>(nowYmd.year());
        if (month > nowMonth)
        {
            year -= 1;
        }
        const date::year_month_day ymd{date::year{year}, date::month{month}, date::day{static_cast<unsigned>(day)}};
        const auto days = date::sys_days{ymd};
        return TimeStamp{
            std::chrono::duration_cast<std::chrono::microseconds>(days.time_since_epoch()) +
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::seconds{(hour * 3600) + (minute * 60) + second}
            )
        };
    };

    SECTION("Space-padded single-digit day (`%e` shape, canonical RFC 3164)")
    {
        TimeStamp out{};
        REQUIRE(TryParseSyslogRfc3164Timestamp("Aug  8 13:25:37", out, REFERENCE_NOW));
        CHECK(out == expected(8, 8, 13, 25, 37));
    }

    SECTION("Zero-padded single-digit day (`%d` shape emitted by some senders)")
    {
        TimeStamp out{};
        REQUIRE(TryParseSyslogRfc3164Timestamp("Aug 08 13:25:37", out, REFERENCE_NOW));
        CHECK(out == expected(8, 8, 13, 25, 37));
    }

    SECTION("Two-digit day")
    {
        TimeStamp out{};
        REQUIRE(TryParseSyslogRfc3164Timestamp("Dec 31 23:59:59", out, REFERENCE_NOW));
        CHECK(out == expected(12, 31, 23, 59, 59));
    }

    SECTION("Every month abbreviation parses")
    {
        static constexpr std::array<std::string_view, 12> STAMPS = {
            "Jan  1 00:00:00",
            "Feb  1 00:00:00",
            "Mar  1 00:00:00",
            "Apr  1 00:00:00",
            "May  1 00:00:00",
            "Jun  1 00:00:00",
            "Jul  1 00:00:00",
            "Aug  1 00:00:00",
            "Sep  1 00:00:00",
            "Oct  1 00:00:00",
            "Nov  1 00:00:00",
            "Dec  1 00:00:00",
        };
        for (unsigned i = 0; i < STAMPS.size(); ++i)
        {
            CAPTURE(STAMPS[i]);
            TimeStamp out{};
            REQUIRE(TryParseSyslogRfc3164Timestamp(STAMPS[i], out, REFERENCE_NOW));
            CHECK(out == expected(i + 1, 1, 0, 0, 0));
        }
    }
}

TEST_CASE("TryParseSyslogRfc3164Timestamp rejects malformed inputs", "[log_processing][syslog_rfc3164_fast_path]")
{
    TimeStamp out{};

    SECTION("Empty and too-short strings")
    {
        CHECK_FALSE(TryParseSyslogRfc3164Timestamp("", out, REFERENCE_NOW));
        CHECK_FALSE(TryParseSyslogRfc3164Timestamp("Aug  8 13:25", out, REFERENCE_NOW));
    }

    SECTION("Unknown month abbreviation")
    {
        CHECK_FALSE(TryParseSyslogRfc3164Timestamp("Xyz  8 13:25:37", out, REFERENCE_NOW));
        // Case-sensitive on purpose -- RFC 3164 §4.1.2 pins the canonical spelling.
        CHECK_FALSE(TryParseSyslogRfc3164Timestamp("aug  8 13:25:37", out, REFERENCE_NOW));
        CHECK_FALSE(TryParseSyslogRfc3164Timestamp("AUG  8 13:25:37", out, REFERENCE_NOW));
    }

    SECTION("Invalid day-of-month")
    {
        CHECK_FALSE(TryParseSyslogRfc3164Timestamp("Feb 30 12:00:00", out, REFERENCE_NOW));
        CHECK_FALSE(TryParseSyslogRfc3164Timestamp("Jan  0 12:00:00", out, REFERENCE_NOW));
        CHECK_FALSE(TryParseSyslogRfc3164Timestamp("Jan 32 12:00:00", out, REFERENCE_NOW));
    }

    SECTION("Invalid hour / minute")
    {
        CHECK_FALSE(TryParseSyslogRfc3164Timestamp("Aug  8 24:00:00", out, REFERENCE_NOW));
        CHECK_FALSE(TryParseSyslogRfc3164Timestamp("Aug  8 12:60:00", out, REFERENCE_NOW));
    }

    SECTION("Trailing garbage after the seconds field")
    {
        // The parser accepts only the exact header; the caller is
        // expected to strip surrounding record framing.
        CHECK_FALSE(TryParseSyslogRfc3164Timestamp("Aug  8 13:25:37 host", out, REFERENCE_NOW));
        CHECK_FALSE(TryParseSyslogRfc3164Timestamp("Aug  8 13:25:37Z", out, REFERENCE_NOW));
    }

    SECTION("Missing separator space after month")
    {
        CHECK_FALSE(TryParseSyslogRfc3164Timestamp("Aug08 13:25:37", out, REFERENCE_NOW));
    }
}

TEST_CASE("ClassifyTimestampFormat maps syslog format strings to the fast path", "[log_processing]")
{
    // The classifier is what wires `%b %e %H:%M:%S` /
    // `%b %d %H:%M:%S` into `TryParseSyslogRfc3164Timestamp`. Both
    // day-padding shapes must land on the same fast-path kind so a
    // sender variance never silently degrades to the slow generic
    // parser.
    CHECK(ClassifyTimestampFormat("%b %e %H:%M:%S") == TimestampFormatKind::SyslogRfc3164NoYear);
    CHECK(ClassifyTimestampFormat("%b %d %H:%M:%S") == TimestampFormatKind::SyslogRfc3164NoYear);
    CHECK(ClassifyTimestampFormat("%b %H:%M:%S") == TimestampFormatKind::Generic);
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

    SECTION("Iso8601T routes to fast path")
    {
        REQUIRE(TryParseTimestamp(
            "2025-04-25T12:34:56", "%FT%T", TimestampFormatKind::Iso8601T, scratch, REFERENCE_NOW, out
        ));
        CHECK(out == TimeStamp{std::chrono::microseconds{1745584496000000}});
    }

    SECTION("Iso8601Space routes to fast path")
    {
        REQUIRE(TryParseTimestamp(
            "2025-04-25 12:34:56", "%F %T", TimestampFormatKind::Iso8601Space, scratch, REFERENCE_NOW, out
        ));
        CHECK(out == TimeStamp{std::chrono::microseconds{1745584496000000}});
    }

    SECTION("Generic routes to date::parse")
    {
        REQUIRE(TryParseTimestamp(
            "2025-04-25T12:34:56+00:00", "%FT%T%Ez", TimestampFormatKind::Generic, scratch, REFERENCE_NOW, out
        ));
        CHECK(out == TimeStamp{std::chrono::microseconds{1745584496000000}});
    }

    SECTION("Generic rejects malformed input")
    {
        CHECK_FALSE(
            TryParseTimestamp("not a date", "%FT%T%Ez", TimestampFormatKind::Generic, scratch, REFERENCE_NOW, out)
        );
    }
}

TEST_CASE("Default parse formats accept Apache error log timestamps", "[log_processing]")
{
    // 2026-03-01 00:00:00 UTC; Apache writes 0, 3, or 6 fraction digits.
    constexpr std::int64_t BASE_US = 1772323200000000;
    const std::vector<std::pair<std::string, std::int64_t>> cases = {
        {"Sun Mar 01 00:00:00 2026", BASE_US},
        {"Sun Mar 01 00:00:00.057 2026", BASE_US + 57000},
        {"Sun Mar 01 00:00:00.290122 2026", BASE_US + 290122},
    };

    for (const auto &[input, expectedUs] : cases)
    {
        CAPTURE(input);
        TimestampParseScratch scratch;
        std::optional<TimeStamp> parsed;
        for (const std::string &format : loglib::DefaultTimeParseFormats())
        {
            TimeStamp out{};
            if (TryParseTimestamp(input, format, ClassifyTimestampFormat(format), scratch, REFERENCE_NOW, out))
            {
                parsed = out;
                break;
            }
        }
        REQUIRE(parsed.has_value());
        CHECK(parsed->time_since_epoch().count() == expectedUs);
    }
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
        const int64_t localMilliseconds = ProcessDefaultTimeZone().ToLocalMilliseconds(originalTimestamp);
        const TimeStamp convertedTimestamp = LocalMillisecondsSinceEpochToTimeStamp(localMilliseconds);

        const int64_t originalMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(originalTimestamp.time_since_epoch()).count();
        const int64_t convertedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(convertedTimestamp.time_since_epoch()).count();

        // Round-trip is millisecond-precision (the int64_t carrier).
        CHECK(originalMs == convertedMs);
    }
}

// `LocalMicrosecondsSinceEpochToUtc` under `Europe/Berlin` (a
// pinned IANA zone rather than the process default for deterministic
// CI). Covers ordinary, fall-back, spring-forward, UTC identity,
// and far-future inputs.
TEST_CASE("LocalMicrosecondsSinceEpochToUtc handles DST transitions", "[log_processing]")
{
    InitializeTimezoneData();
    const auto berlin = TimeZoneContext::Load(FindTestTzdata(), "Europe/Berlin");
    CHECK(berlin.IanaName() == "Europe/Berlin");

    // Berlin DST 2024: spring forward 2024-03-31 02:00 -> 03:00
    // CEST (02:30 does not exist); fall back 2024-10-27 03:00 ->
    // 02:00 CET (02:30 exists twice).
    const auto naiveLocalMicros = [](int year, unsigned month, unsigned day, int hour, int minute, int second) {
        const auto ymd = date::year{year} / date::month{month} / date::day{day};
        const auto sysDays = date::sys_days{ymd};
        const auto wallClock =
            sysDays + std::chrono::hours{hour} + std::chrono::minutes{minute} + std::chrono::seconds{second};
        return std::chrono::duration_cast<std::chrono::microseconds>(wallClock.time_since_epoch()).count();
    };

    SECTION("Ordinary hour round-trips through the zone offset")
    {
        // 2024-04-01 12:00 Berlin (CEST +02:00) -> 10:00 UTC.
        const int64_t localMicros = naiveLocalMicros(2024, 4, 1, 12, 0, 0);
        const int64_t expectedUtcMicros = naiveLocalMicros(2024, 4, 1, 10, 0, 0);
        const int64_t got = LocalMicrosecondsSinceEpochToUtc(localMicros, berlin);
        CHECK(got == expectedUtcMicros);
    }

    SECTION("Fall-back ambiguous hour resolves to the earlier candidate")
    {
        // 2024-10-27 02:30 Berlin occurs twice; `choose::earliest`
        // picks CEST (+02:00) -> 00:30 UTC.
        const int64_t localMicros = naiveLocalMicros(2024, 10, 27, 2, 30, 0);
        const int64_t earlierUtcMicros = naiveLocalMicros(2024, 10, 27, 0, 30, 0);
        const int64_t got = LocalMicrosecondsSinceEpochToUtc(localMicros, berlin);
        CHECK(got == earlierUtcMicros);
    }

    SECTION("Spring-forward gap hour snaps to the transition boundary")
    {
        // 2024-03-31 02:30 Berlin does not exist. `choose::earliest`
        // returns the transition boundary (01:00 UTC = the instant
        // clocks jumped forward), not `nonexistent_local_time`.
        // Pinning the exact value guards against a future date
        // library changing this semantic.
        const int64_t localMicros = naiveLocalMicros(2024, 3, 31, 2, 30, 0);
        const int64_t transitionBoundaryUtc = naiveLocalMicros(2024, 3, 31, 1, 0, 0);
        const int64_t got = LocalMicrosecondsSinceEpochToUtc(localMicros, berlin);
        CHECK(got == transitionBoundaryUtc);
    }

    SECTION("UTC context passes the value through unchanged")
    {
        const int64_t localMicros = naiveLocalMicros(2024, 4, 1, 12, 0, 0);
        const int64_t got = LocalMicrosecondsSinceEpochToUtc(localMicros, TimeZoneContext::Utc());
        CHECK(got == localMicros);
    }

    SECTION("Far-future / far-past instants degrade gracefully")
    {
        // Well past the tzdata transition table. `to_sys` may
        // throw a non-DST exception; the catch keeps the helper
        // exception-safe by yielding the naive value. Either way
        // the shift stays within +/-14 h.
        const int64_t farFutureMicros = naiveLocalMicros(9999, 12, 31, 23, 59, 59);
        const int64_t gotFuture = LocalMicrosecondsSinceEpochToUtc(farFutureMicros, berlin);
        constexpr int64_t MAX_ZONE_OFFSET_MICROS = 14LL * 3600LL * 1'000'000LL;
        CHECK(std::llabs(gotFuture - farFutureMicros) <= MAX_ZONE_OFFSET_MICROS);
    }
}

TEST_CASE("TimeZoneContext::LocalMillisecondsToUtc handles DST transitions", "[log_processing]")
{
    InitializeTimezoneData();
    const auto berlin = TimeZoneContext::Load(FindTestTzdata(), "Europe/Berlin");
    CHECK(berlin.IanaName() == "Europe/Berlin");

    const auto naiveLocalMicros = [](int year, unsigned month, unsigned day, int hour, int minute, int second) {
        const auto ymd = date::year{year} / date::month{month} / date::day{day};
        const auto sysDays = date::sys_days{ymd};
        const auto wallClock =
            sysDays + std::chrono::hours{hour} + std::chrono::minutes{minute} + std::chrono::seconds{second};
        return std::chrono::duration_cast<std::chrono::microseconds>(wallClock.time_since_epoch()).count();
    };
    const auto naiveLocalMillis = [&](int year, unsigned month, unsigned day, int hour, int minute, int second) {
        return naiveLocalMicros(year, month, day, hour, minute, second) / 1000;
    };
    const auto utcStamp = [&](int year, unsigned month, unsigned day, int hour, int minute, int second) {
        return TimeStamp{std::chrono::microseconds{naiveLocalMicros(year, month, day, hour, minute, second)}};
    };

    SECTION("Ordinary hour round-trips through the zone offset")
    {
        const TimeStamp got = berlin.LocalMillisecondsToUtc(naiveLocalMillis(2024, 4, 1, 12, 0, 0));
        CHECK(got == utcStamp(2024, 4, 1, 10, 0, 0));
    }

    SECTION("Fall-back ambiguous hour resolves to the earlier candidate")
    {
        CHECK_NOTHROW(berlin.LocalMillisecondsToUtc(naiveLocalMillis(2024, 10, 27, 2, 30, 0)));
        const TimeStamp got = berlin.LocalMillisecondsToUtc(naiveLocalMillis(2024, 10, 27, 2, 30, 0));
        CHECK(got == utcStamp(2024, 10, 27, 0, 30, 0));
    }

    SECTION("Spring-forward gap hour snaps to the transition boundary")
    {
        CHECK_NOTHROW(berlin.LocalMillisecondsToUtc(naiveLocalMillis(2024, 3, 31, 2, 30, 0)));
        const TimeStamp got = berlin.LocalMillisecondsToUtc(naiveLocalMillis(2024, 3, 31, 2, 30, 0));
        CHECK(got == utcStamp(2024, 3, 31, 1, 0, 0));
    }

    SECTION("UTC context passes the value through unchanged")
    {
        const int64_t localMillis = naiveLocalMillis(2024, 4, 1, 12, 0, 0);
        const TimeStamp got = TimeZoneContext::Utc().LocalMillisecondsToUtc(localMillis);
        CHECK(got == TimeStamp{std::chrono::milliseconds{localMillis}});
    }
}

TEST_CASE("TimeZoneContext::Load throws for an unavailable zone", "[log_processing]")
{
    InitializeTimezoneData();
    CHECK_THROWS_AS(TimeZoneContext::Load(FindTestTzdata(), "Not/ARealZone"), std::runtime_error);
}

TEST_CASE(
    "TryParseSyslogRfc3164Timestamp year rollover uses the reference instant",
    "[log_processing][syslog_rfc3164_fast_path]"
)
{
    TimeStamp out{};
    const auto january = date::sys_days{date::year{2026} / date::January / 15};
    REQUIRE(TryParseSyslogRfc3164Timestamp("Dec 31 23:59:59", out, january));
    const date::year_month_day ymdJan{date::floor<date::days>(out)};
    CHECK(static_cast<int>(ymdJan.year()) == 2025);

    const auto december = date::sys_days{date::year{2026} / date::December / 15};
    REQUIRE(TryParseSyslogRfc3164Timestamp("Dec 31 23:59:59", out, december));
    const date::year_month_day ymdDec{date::floor<date::days>(out)};
    CHECK(static_cast<int>(ymdDec.year()) == 2026);
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

namespace
{

} // namespace

TEST_CASE("OffsetWindowAt agrees with LocalMicrosecondsToUtc across a year", "[time_zone][offset_window]")
{
    const auto berlin = TimeZoneContext::Load(FindTestTzdata(), "Europe/Berlin");
    constexpr std::int64_t HALF_HOUR_US = 1'800'000'000;
    constexpr std::int64_t START_US = 1'704'067'200'000'000; // 2024-01-01T00:00:00
    constexpr int STEPS = 2 * 24 * 366;

    int inWindow = 0;
    for (int step = 0; step < STEPS; ++step)
    {
        const std::int64_t local = START_US + (step * HALF_HOUR_US);
        const LocalOffsetWindow window = berlin.OffsetWindowAt(local);
        if (window.Contains(local))
        {
            ++inWindow;
            INFO("local " << local);
            REQUIRE(local - window.utcOffset == berlin.LocalMicrosecondsToUtc(local));
        }
    }
    CHECK(inWindow > STEPS / 2);

    // 2024-03-31T02:30:00 does not exist in Berlin; 2024-10-27T02:30:00 happens twice.
    constexpr std::int64_t GAP_US = 1'711'852'200'000'000;
    constexpr std::int64_t FOLD_US = 1'729'996'200'000'000;
    CHECK_FALSE(berlin.OffsetWindowAt(GAP_US).Contains(GAP_US));
    CHECK_FALSE(berlin.OffsetWindowAt(FOLD_US).Contains(FOLD_US));

    const TimeZoneContext utc;
    CHECK(utc.OffsetWindowAt(START_US).Contains(START_US));
    CHECK(utc.OffsetWindowAt(START_US).utcOffset == 0);
}

TEST_CASE("Generic timestamps never read fraction digits as an offset", "[timestamp][generic]")
{
    const std::string format = "%d-%m-%Y %T%Ez";
    TimestampParseScratch scratch;
    TimeStamp parsed;

    for (const std::string text : {"12-03-2024 10:00:00.1234567", "12-03-2024 10:00:00.12345678"})
    {
        INFO(text);
        CHECK_FALSE(TryParseGenericTimestamp(text, format, scratch, parsed));
    }

    REQUIRE(TryParseGenericTimestamp("12-03-2024 10:00:00.123456+01:00", format, scratch, parsed));
    const TimeStamp expected = parsed;
    REQUIRE(TryParseGenericTimestamp("12-03-2024 10:00:00.123456789+01:00", format, scratch, parsed));
    CHECK(parsed == expected);
}

TEST_CASE("IsNaiveTimestamp finds values that name no zone", "[timestamp][naive]")
{
    CHECK(IsNaiveTimestamp("2024-01-01T10:00:00", "%FT%T"));
    CHECK_FALSE(IsNaiveTimestamp("2024-01-01T10:00:00Z", "%FT%T"));
    CHECK_FALSE(IsNaiveTimestamp("2024-01-01T10:00:00+01:00", "%FT%T%Ez"));
    CHECK(IsNaiveTimestamp("Jan  1 10:00:00", "%b %e %H:%M:%S"));
}

TEST_CASE("ParseQuery reads time bounds without a zone in the zone it is given", "[query_parser][naive]")
{
    const auto tokyo = TimeZoneContext::Load(FindTestTzdata(), "Asia/Tokyo");
    const auto berlin = TimeZoneContext::Load(FindTestTzdata(), "Europe/Berlin");
    const auto bound = [](std::string_view text, const TimeZoneContext &zone) -> std::optional<std::int64_t> {
        const auto parsed = ParseQuery("ts IN [" + std::string(text) + "..]", zone);
        REQUIRE(parsed.has_value());
        return std::get<FilterExpression::Leaf>(parsed->node).rule.filterBegin;
    };
    const auto utcBound = bound("2024-01-01T01:00:00Z", TimeZoneContext{});

    SECTION("A zone-less time is read in the given zone")
    {
        CHECK(bound("2024-01-01T10:00:00", tokyo) == utcBound);
        CHECK(bound("2024-01-01 10:00:00", tokyo) == utcBound);
    }

    SECTION("A time that names its zone ignores the given zone")
    {
        CHECK(bound("2024-01-01T01:00:00Z", tokyo) == utcBound);
        CHECK(bound("2024-01-01T10:00:00+09:00", berlin) == utcBound);
    }

    SECTION("The same text gives different bounds in different zones, whatever the process zone is")
    {
        const auto inUtc = bound("2024-01-01T10:00:00", TimeZoneContext{});
        const auto inBerlin = bound("2024-01-01T10:00:00", berlin);
        const auto inTokyo = bound("2024-01-01T10:00:00", tokyo);
        REQUIRE(inUtc.has_value());
        REQUIRE(inBerlin.has_value());
        REQUIRE(inTokyo.has_value());
        constexpr std::int64_t HOUR_US = 3'600'000'000;
        CHECK(*inUtc - *inBerlin == HOUR_US);
        CHECK(*inUtc - *inTokyo == 9 * HOUR_US);
    }
}
