#include <loglib/core/log_value.hpp>
#include <loglib/time/timestamp_parsing.hpp>

#include <catch2/catch_all.hpp>
#include <date/date.h>

#include <chrono>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using namespace loglib;

namespace
{

std::chrono::system_clock::time_point ReferenceTime()
{
    using namespace std::chrono;
    return system_clock::time_point{sys_days{year{2024} / June / 15}};
}

/** @brief The result of one parse: whether it succeeded, and the time if it did. */
struct Outcome
{
    bool parsed = false;
    std::int64_t micros = 0;

    bool operator==(const Outcome &) const = default;
};

Outcome ParseCompiled(const TimestampFormat &format, std::string_view text)
{
    TimestampParseScratch scratch;
    TimeStamp out{};
    const bool parsed = format.Parse(text, scratch, ReferenceTime(), out);
    return Outcome{.parsed = parsed, .micros = parsed ? out.time_since_epoch().count() : 0};
}

Outcome ParseWithDateLibrary(const std::string &format, std::string_view text)
{
    TimestampParseScratch scratch;
    TimeStamp out{};
    const bool parsed = TryParseGenericTimestamp(text, format, scratch, out);
    return Outcome{.parsed = parsed, .micros = parsed ? out.time_since_epoch().count() : 0};
}

/** @brief Writes a time with a date::format format, as a program writing a log would. */
std::string FormatTimestampForTest(TimeStamp value, const std::string &format)
{
    return date::format(format.c_str(), value);
}

/** @brief Checks that the compiled format and date::parse agree on text. */
void CheckAgrees(const std::string &formatText, std::string_view text)
{
    const TimestampFormat format(formatText);
    REQUIRE(format.Kind() == TimestampFormatKind::Compiled);
    const Outcome compiled = ParseCompiled(format, text);
    const Outcome reference = ParseWithDateLibrary(formatText, text);
    INFO("format '" << formatText << "', text '" << text << "'");
    CHECK(compiled.parsed == reference.parsed);
    CHECK(compiled.micros == reference.micros);
}

} // namespace

TEST_CASE("A compiled format reads the same as date::parse on edge cases", "[timestamp_format]")
{
    struct Case
    {
        std::string format;
        std::vector<std::string_view> texts;
    };
    const std::vector<Case> cases = {
        {
            .format = "%Y-%m-%dT%H:%M:%S",
            .texts =
                {
                    "2024-05-01T12:30:45",
                    "2024-5-1T1:2:3",
                    "2024-05-01T12:30:45.5",
                    "2024-05-01T12:30:45.123456",
                    "2024-05-01T12:30:45.1234567",
                    "2024-05-01T12:30:45.",
                    "2024-05-01T12:30:.5",
                    "2024-05-01T12:30:60",
                    "2024-05-01T24:00:00",
                    "2024-05-01T12:60:00",
                    "2024-02-30T12:00:00",
                    "2024-02-29T12:00:00",
                    "2023-02-29T12:00:00",
                    "-123-05-01T12:30:45",
                    "+202-05-01T12:30:45",
                    "02024-05-01T12:30:45",
                    "2024-05-01T12:30:45Z",
                    "2024-05-01T12:30:4",
                    "2024-05-01T12:30",
                    "",
                    "2024-13-01T00:00:00",
                    "2024-00-01T00:00:00",
                    "2024-05-00T00:00:00",
                    "2024-05-01T12:30:045",
                    "2024-05-01T12:30:45.1.2",
                },
        },
        {
            .format = "%Y-%m-%d %T",
            .texts =
                {
                    "2024-05-01 12:30:45",
                    "2024-05-01  12:30:45",
                    "2024-05-0112:30:45",
                    "2024-05-01\t12:30:45",
                    "2024-05-01 12:30:45 ",
                    "2024-05-01 12:30:45.123456789",
                },
        },
        {
            .format = "%d/%b/%Y:%H:%M:%S %z",
            .texts =
                {
                    "10/Oct/2000:13:55:36 -0700",
                    "10/Oct/2000:13:55:36 +0000",
                    "10/Oct/2000:13:55:36 +05",
                    "10/Oct/2000:13:55:36 0530",
                    "10/Oct/2000:13:55:36 -07:00",
                    "10/Oct/2000:13:55:36 -070",
                    "10/Oct/2000:13:55:36 +9999",
                    "10/Oct/2000:13:55:36",
                    "10/oct/2000:13:55:36 -0700",
                    "10/October/2000:13:55:36 -0700",
                    "10/Okt/2000:13:55:36 -0700",
                    "10/1/2000:13:55:36 -0700",
                    "1/Oct/2000:13:55:36 -0700",
                    "10/Oct/2000:13:55:36 -0700 ",
                },
        },
        {
            .format = "%Y-%m-%dT%H:%M:%S%Ez",
            .texts =
                {
                    "2024-05-01T12:30:45+05:30",
                    "2024-05-01T12:30:45+5:30",
                    "2024-05-01T12:30:45+05",
                    "2024-05-01T12:30:45+0530",
                    "2024-05-01T12:30:45-00:00",
                    "2024-05-01T12:30:45+05:3",
                    "2024-05-01T12:30:45.999999-11:59",
                    "2024-05-01T12:30:45Z",
                },
        },
        {
            .format = "%a %b %d %H:%M:%S %Y",
            .texts =
                {
                    "Sun Mar 01 00:00:00 2026",
                    "Sun Mar 01 00:00:00.057 2026",
                    "Mon Mar 01 00:00:00 2026",
                    "Sun Mar  1 00:00:00 2026",
                    "Sun Mar 1 00:00:00 2026",
                    "Sunday Mar 01 00:00:00 2026",
                    "sun Mar 01 00:00:00 2026",
                    "Sun Mar 01 00:00:00 26",
                    "5un Mar 01 00:00:00 2026",
                },
        },
        {
            .format = "%e %B %Y %T %Z",
            .texts =
                {
                    " 1 Mar 2026 07:08:09 UTC",
                    "1 Mar 2026 07:08:09 Europe/Prague",
                    "1 Mar 2026 07:08:09 ",
                    "1 Mar 2026 07:08:09 +05",
                    "1 Mar 2026 07:08:09 UTC!",
                    "1 March 2026 07:08:09 UTC",
                },
        },
        {
            .format = "[%Y.%m.%d %H%M%S]",
            .texts =
                {
                    "[2024.05.01 123045]",
                    "[2024.05.01 12304]",
                    "[2024.5.1 1230]",
                    "[2024.05.01 123045.5]",
                    "[2024.05.1234567 123045]",
                },
        },
        {
            .format = "%Y%m%d %%%H",
            .texts = {
                "20240501 %12",
                "2024051 %12",
                "20240501 12",
                "20240501 %%12",
            },
        },
    };

    for (const Case &testCase : cases)
    {
        for (const std::string_view text : testCase.texts)
        {
            CheckAgrees(testCase.format, text);
        }
    }
}

TEST_CASE("A compiled format ends at a null character, as date::parse does", "[timestamp_format]")
{
    // date::parse takes the format as a C string, so whatever follows a null character is not part of it.
    const std::string format("%F\0\0 %H", 7);
    CheckAgrees(format, "08-2-2");
    CheckAgrees(format, "2024-05-01 12");
}

TEST_CASE("A compiled format reads the same as date::parse on random text", "[timestamp_format]")
{
    const std::vector<std::string> formats = {
        "%Y-%m-%dT%H:%M:%S",
        "%Y-%m-%d %H:%M:%S%z",
        "%Y-%m-%dT%H:%M:%S%Ez",
        "%d/%b/%Y:%H:%M:%S %z",
        "%a %b %d %H:%M:%S %Y",
        "%e %b %Y %T %Z",
        "%Y%m%d%H%M%S",
        "%m.%d.%Y %H:%M",
    };
    // Characters that the formats above use, so a mutation often still nearly matches.
    constexpr std::string_view ALPHABET = "0123456789 -+:./%TZabeMnOrSuJ\t";

    // A fixed seed keeps a failure reproducible.
    std::mt19937 random(20261110U); // NOLINT(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    const auto pick = [&random](std::size_t count) {
        return std::uniform_int_distribution<std::size_t>(0, count - 1)(random);
    };

    for (const std::string &formatText : formats)
    {
        const TimestampFormat format(formatText);
        REQUIRE(format.Kind() == TimestampFormatKind::Compiled);
        for (int i = 0; i < 4000; ++i)
        {
            // Start from a valid value written with the format, then change a few characters.
            const auto seconds = std::chrono::seconds{static_cast<std::int64_t>(pick(4'000'000'000U))};
            const auto micros = std::chrono::microseconds{static_cast<std::int64_t>(pick(1'000'000U))};
            const TimeStamp value{std::chrono::duration_cast<std::chrono::microseconds>(seconds) + micros};
            std::string text = FormatTimestampForTest(value, formatText);
            const std::size_t mutations = pick(4);
            for (std::size_t m = 0; m < mutations && !text.empty(); ++m)
            {
                const std::size_t at = pick(text.size());
                switch (pick(3))
                {
                case 0:
                    text[at] = ALPHABET[pick(ALPHABET.size())];
                    break;
                case 1:
                    text.insert(at, 1, ALPHABET[pick(ALPHABET.size())]);
                    break;
                default:
                    text.erase(at, 1);
                    break;
                }
            }

            const Outcome compiled = ParseCompiled(format, text);
            const Outcome reference = ParseWithDateLibrary(formatText, text);
            if (compiled != reference)
            {
                INFO("format '" << formatText << "', text '" << text << "'");
                CHECK(compiled.parsed == reference.parsed);
                CHECK(compiled.micros == reference.micros);
            }
        }
    }
}
