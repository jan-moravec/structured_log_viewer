#include <loglib/core/log_value.hpp>
#include <loglib/time/timestamp_parsing.hpp>

#include <catch2/catch_all.hpp>

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

using namespace loglib;

namespace
{

/** @brief Microseconds since the epoch of a UTC date and time. */
std::int64_t Utc(int year, unsigned month, unsigned day, int hour, int minute, int second, int micros = 0)
{
    const auto days =
        std::chrono::sys_days{std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}};
    const auto total = std::chrono::duration_cast<std::chrono::microseconds>(days.time_since_epoch()) +
                       std::chrono::hours{hour} + std::chrono::minutes{minute} + std::chrono::seconds{second} +
                       std::chrono::microseconds{micros};
    return total.count();
}

/** @brief One timestamp as a program writes it, the format that reads it, and the UTC time it stands for. */
struct CorpusEntry
{
    std::string_view source;
    std::string_view text;
    std::string_view format;
    std::int64_t expectedUtcMicros;
};

/** @brief The "now" for formats without a year: 2024-06-01 00:00:00 UTC. */
std::chrono::system_clock::time_point ReferenceTime()
{
    return std::chrono::system_clock::time_point{std::chrono::duration_cast<std::chrono::system_clock::duration>(
        std::chrono::microseconds{Utc(2024, 6, 1, 0, 0, 0)}
    )};
}

// The ISO 8601 families all use one of these formats.
constexpr std::string_view ISO_T = "%FT%T";
constexpr std::string_view ISO_T_OFFSET = "%FT%T%Ez";
constexpr std::string_view ISO_SPACE = "%F %T";
constexpr std::string_view SYSLOG_BSD = "%b %e %H:%M:%S";
constexpr std::string_view COMMON_LOG = "%d/%b/%Y:%H:%M:%S %z";
constexpr std::string_view NGINX_ERROR = "%Y/%m/%d %H:%M:%S";

std::vector<CorpusEntry> Corpus()
{
    return {
        // Go, time.RFC3339Nano. Digits past microseconds are cut off.
        {"go", "2024-05-01T12:30:45.123456789Z", ISO_T, Utc(2024, 5, 1, 12, 30, 45, 123456)},
        {"go", "2024-05-01T12:30:45Z", ISO_T, Utc(2024, 5, 1, 12, 30, 45)},
        {"go", "2024-05-01T14:30:45.5+02:00", ISO_T_OFFSET, Utc(2024, 5, 1, 12, 30, 45, 500000)},
        {"go", "2024-05-01T05:30:45.000000001-07:00", ISO_T_OFFSET, Utc(2024, 5, 1, 12, 30, 45)},
        {"go", "2023-12-31T23:59:59.999999999Z", ISO_T, Utc(2023, 12, 31, 23, 59, 59, 999999)},

        // Uber Zap: the console encoder and the production ISO 8601 encoder.
        {"zap", "2024-04-28T04:02:03.123Z", ISO_T, Utc(2024, 4, 28, 4, 2, 3, 123000)},
        {"zap", "2024-04-28T04:02:04.456-04:00", ISO_T_OFFSET, Utc(2024, 4, 28, 8, 2, 4, 456000)},
        {"zap", "2024-04-28T04:02:05Z", ISO_T, Utc(2024, 4, 28, 4, 2, 5)},
        {"zap", "2024-04-28T04:02:06.789Z", ISO_T, Utc(2024, 4, 28, 4, 2, 6, 789000)},
        {"zap", "2024-04-28T04:02:07.000+0200", ISO_T_OFFSET, Utc(2024, 4, 28, 2, 2, 7)},

        // Java: the Logback default date, the log4j2 ISO8601 date, and the JSON layouts. The plain dates write the
        // milliseconds after a comma.
        {"java", "2024-04-28 04:02:05,789", ISO_SPACE, Utc(2024, 4, 28, 4, 2, 5, 789000)},
        {"java", "2024-04-28 23:59:59,999", ISO_SPACE, Utc(2024, 4, 28, 23, 59, 59, 999000)},
        {"java", "2024-04-28T04:02:04,456", ISO_T, Utc(2024, 4, 28, 4, 2, 4, 456000)},
        {"java", "2024-04-28T04:02:04.456Z", ISO_T, Utc(2024, 4, 28, 4, 2, 4, 456000)},
        {"java", "2024-04-28T04:02:06.000+02:00", ISO_T_OFFSET, Utc(2024, 4, 28, 2, 2, 6)},

        // Python: the default asctime of the logging module, with a comma before the milliseconds, and
        // datetime.isoformat.
        {"python", "2024-04-28 04:02:03,123", ISO_SPACE, Utc(2024, 4, 28, 4, 2, 3, 123000)},
        {"python", "2024-12-31 23:59:59,999", ISO_SPACE, Utc(2024, 12, 31, 23, 59, 59, 999000)},
        {"python", "2024-01-01 00:00:00,001", ISO_SPACE, Utc(2024, 1, 1, 0, 0, 0, 1000)},
        {"python", "2024-02-29 12:00:00,500", ISO_SPACE, Utc(2024, 2, 29, 12, 0, 0, 500000)},
        {"python", "2024-07-04T10:11:12.131415", ISO_T, Utc(2024, 7, 4, 10, 11, 12, 131415)},

        // RFC 3164 syslog. The year comes from the reference time, and a month after the reference month is last year.
        {"syslog3164", "Apr 28 04:02:03", SYSLOG_BSD, Utc(2024, 4, 28, 4, 2, 3)},
        {"syslog3164", "Jun 27 01:47:20", SYSLOG_BSD, Utc(2024, 6, 27, 1, 47, 20)},
        {"syslog3164", "Jan  4 10:23:26", SYSLOG_BSD, Utc(2024, 1, 4, 10, 23, 26)},
        {"syslog3164", "Dec 31 23:59:59", SYSLOG_BSD, Utc(2023, 12, 31, 23, 59, 59)},
        {"syslog3164", "Feb 29 12:00:00", SYSLOG_BSD, Utc(2024, 2, 29, 12, 0, 0)},

        // RFC 5424 syslog.
        {"syslog5424", "2017-04-27T07:50:47.381967+02:00", ISO_T_OFFSET, Utc(2017, 4, 27, 5, 50, 47, 381967)},
        {"syslog5424", "2017-04-27T07:59:12+02:00", ISO_T_OFFSET, Utc(2017, 4, 27, 5, 59, 12)},
        {"syslog5424", "2017-04-27T08:09:01Z", ISO_T, Utc(2017, 4, 27, 8, 9, 1)},
        {"syslog5424", "2024-02-29T23:59:59.999999-05:00", ISO_T_OFFSET, Utc(2024, 3, 1, 4, 59, 59, 999999)},
        {"syslog5424", "2017-04-27T07:50:47.381967+00:00", ISO_T_OFFSET, Utc(2017, 4, 27, 7, 50, 47, 381967)},

        // nginx and Apache access logs: the common log format with a numeric zone.
        {"nginx_access", "10/Oct/2000:13:55:36 -0700", COMMON_LOG, Utc(2000, 10, 10, 20, 55, 36)},
        {"nginx_access", "23/Dec/2014:21:20:35 +0000", COMMON_LOG, Utc(2014, 12, 23, 21, 20, 35)},
        {"nginx_access", "01/Jan/2024:00:00:00 +0000", COMMON_LOG, Utc(2024, 1, 1, 0, 0, 0)},
        {"nginx_access", "29/Feb/2024:23:30:00 +0100", COMMON_LOG, Utc(2024, 2, 29, 22, 30, 0)},
        {"nginx_access", "15/Aug/2023:08:00:00 -0330", COMMON_LOG, Utc(2023, 8, 15, 11, 30, 0)},

        // nginx error log: no zone, read as UTC here.
        {"nginx_error", "2024/04/28 04:02:03", NGINX_ERROR, Utc(2024, 4, 28, 4, 2, 3)},
        {"nginx_error", "2024/04/28 04:02:04", NGINX_ERROR, Utc(2024, 4, 28, 4, 2, 4)},
        {"nginx_error", "2024/12/31 23:59:59", NGINX_ERROR, Utc(2024, 12, 31, 23, 59, 59)},
        {"nginx_error", "2024/01/01 00:00:00", NGINX_ERROR, Utc(2024, 1, 1, 0, 0, 0)},
        {"nginx_error", "2024/02/29 12:34:56", NGINX_ERROR, Utc(2024, 2, 29, 12, 34, 56)},
    };
}

} // namespace

TEST_CASE("Timestamps as real programs write them are read to the expected UTC time", "[timestamp_corpus]")
{
    TimestampParseScratch scratch;
    for (const CorpusEntry &entry : Corpus())
    {
        const std::string format(entry.format);
        const TimestampFormatKind kind = ClassifyTimestampFormat(format);
        INFO(entry.source << ": " << entry.text << " with " << entry.format);

        TimeStamp parsed;
        REQUIRE(TryParseTimestamp(entry.text, format, kind, scratch, ReferenceTime(), parsed));
        CHECK(parsed.time_since_epoch().count() == entry.expectedUtcMicros);
    }
}

TEST_CASE("The corpus holds at least five lines for each source", "[timestamp_corpus]")
{
    std::vector<std::pair<std::string_view, int>> counts;
    for (const CorpusEntry &entry : Corpus())
    {
        bool found = false;
        for (auto &[source, count] : counts)
        {
            if (source == entry.source)
            {
                ++count;
                found = true;
            }
        }
        if (!found)
        {
            counts.emplace_back(entry.source, 1);
        }
    }
    CHECK(counts.size() == 8);
    for (const auto &[source, count] : counts)
    {
        INFO(source);
        CHECK(count >= 5);
    }
}

namespace
{

/** @brief Whether the text parses with the format to the expected UTC time. */
bool ParsesTo(std::string_view text, std::string_view format, std::int64_t expectedUtcMicros)
{
    TimestampParseScratch scratch;
    TimeStamp parsed;
    const std::string formatText(format);
    return TryParseTimestamp(text, formatText, ClassifyTimestampFormat(formatText), scratch, ReferenceTime(), parsed) &&
           parsed.time_since_epoch().count() == expectedUtcMicros;
}

} // namespace

// Known gap: the klog header is Lmmdd hh:mm:ss.uuuuuu. It has no year, and the general parser fails on a format that
// leaves the year out, so these timestamps stay text. Only the RFC 3164 header has a parser that fills the year in.
// The expected values use the same rule as that header: a month after the reference month is last year. This test
// passes the day a klog or glog format is read.
TEST_CASE("Kubernetes klog timestamps are read with a month-day format", "[timestamp_corpus][!shouldfail]")
{
    constexpr std::string_view KLOG = "%m%d %H:%M:%S";
    CHECK(ParsesTo("0501 12:30:45.123456", KLOG, Utc(2024, 5, 1, 12, 30, 45, 123456)));
    CHECK(ParsesTo("0101 00:00:00.000001", KLOG, Utc(2024, 1, 1, 0, 0, 0, 1)));
    CHECK(ParsesTo("1231 23:59:59.999999", KLOG, Utc(2023, 12, 31, 23, 59, 59, 999999)));
    CHECK(ParsesTo("0229 12:00:00.500000", KLOG, Utc(2024, 2, 29, 12, 0, 0, 500000)));
    CHECK(ParsesTo("0704 10:11:12.131415", KLOG, Utc(2023, 7, 4, 10, 11, 12, 131415)));
}
