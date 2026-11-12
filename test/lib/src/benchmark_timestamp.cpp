// Benchmarks for reading timestamp text. The streaming benchmarks hide this cost, because their generated timestamps
// repeat for many lines in a row and the last parsed text is cached. Here every value is different, so each one is
// parsed. Each format runs twice: through TimestampFormat, which reads it with hand-written steps, and through
// date::parse, which every format used before the steps existed. The formatting benchmarks write times as display text,
// in UTC and in a zone with daylight saving time.

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"

#include "common.hpp"

#include <loglib/time/time_zone_context.hpp>
#include <loglib/time/timestamp_parsing.hpp>

#include <benchmark/benchmark.h>
#include <date/date.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bench
{

namespace
{

using namespace loglib;

constexpr std::size_t VALUE_COUNT = 10'000;

/** @brief VALUE_COUNT different times written with format, 1.7 s apart so every second field changes. */
std::vector<std::string> MakeValues(const std::string &format)
{
    std::vector<std::string> values;
    values.reserve(VALUE_COUNT);
    const TimeStamp start{std::chrono::seconds{1767225600}};
    for (std::size_t i = 0; i < VALUE_COUNT; ++i)
    {
        const TimeStamp value = start + std::chrono::milliseconds{1700 * static_cast<std::int64_t>(i)};
        values.push_back(date::format(format.c_str(), date::floor<std::chrono::seconds>(value)));
    }
    return values;
}

/** @brief Parses every value once per iteration with parse, and fails the benchmark if one does not parse. */
template <typename Parse> void RunParse(benchmark::State &state, const std::vector<std::string> &values, Parse parse)
{
    TimestampParseScratch scratch;
    for (const auto _ : state)
    {
        std::int64_t sum = 0;
        for (const std::string &value : values)
        {
            TimeStamp out{};
            if (!parse(value, scratch, out))
            {
                Fail(state, "could not parse " + value);
                return;
            }
            sum += out.time_since_epoch().count();
        }
        benchmark::DoNotOptimize(sum);
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * values.size()));
}

void RegisterFormat(const std::string &label, const std::string &format)
{
    const auto values = MakeLazy<std::vector<std::string>>([format] {
        return std::make_unique<std::vector<std::string>>(MakeValues(format));
    });
    const std::string count = " 10'000 " + label + " times";

    RegisterSmall(
        "TimestampFormat::Parse" + count,
        [values, format](benchmark::State &state) {
            const TimestampFormat prepared(format);
            const auto reference = std::chrono::system_clock::now();
            RunParse(
                state, values->Get(), [&](const std::string &value, TimestampParseScratch &scratch, TimeStamp &out) {
                    return prepared.Parse(value, scratch, reference, out);
                }
            );
        },
        benchmark::kMicrosecond
    );
    RegisterSmall(
        "date::parse" + count,
        [values, format](benchmark::State &state) {
            RunParse(
                state, values->Get(), [&](const std::string &value, TimestampParseScratch &scratch, TimeStamp &out) {
                    return TryParseGenericTimestamp(value, format, scratch, out);
                }
            );
        },
        benchmark::kMicrosecond
    );
}

/** @brief VALUE_COUNT times 53 minutes apart, about a year, so a zone with daylight saving time changes its offset. */
std::vector<TimeStamp> MakeTimes()
{
    std::vector<TimeStamp> times;
    times.reserve(VALUE_COUNT);
    const TimeStamp start{std::chrono::seconds{1767225600}};
    for (std::size_t i = 0; i < VALUE_COUNT; ++i)
    {
        times.push_back(start + std::chrono::minutes{53 * static_cast<std::int64_t>(i)} + std::chrono::milliseconds{i});
    }
    return times;
}

/** @brief Formats every time once per iteration with the date and time format, and checks the length of the text. */
void RegisterFormatting(const std::string &label, const std::string &zoneName)
{
    const auto times =
        MakeLazy<std::vector<TimeStamp>>([] { return std::make_unique<std::vector<TimeStamp>>(MakeTimes()); });
    RegisterSmall(
        "TimeZoneContext::Format 10'000 times (" + label + ")",
        [times, zoneName](benchmark::State &state) {
            TimeZoneContext zone;
            if (!zoneName.empty())
            {
                InitializeTimezoneData();
                zone = TimeZoneContext::Load(FindTestTzdata(), zoneName);
            }
            const std::vector<TimeStamp> &values = times->Get();
            std::size_t length = 0;
            for (const auto _ : state)
            {
                length = 0;
                for (const TimeStamp value : values)
                {
                    length += zone.Format(value, "%F %T").size();
                }
                benchmark::DoNotOptimize(length);
            }
            // Every text is year-month-day, hours:minutes:seconds and three digits of milliseconds.
            if (length != values.size() * std::string_view{"2026-01-01 00:00:00.000"}.size())
            {
                Fail(state, "a formatted time has an unexpected length");
                return;
            }
            state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * values.size()));
        },
        benchmark::kMicrosecond
    );
}

} // namespace

void RegisterTimestampBenchmarks()
{
    RegisterFormatting("UTC", "");
    RegisterFormatting("Europe/Berlin", "Europe/Berlin");
    RegisterFormat("Apache access", "%d/%b/%Y:%H:%M:%S %z");
    RegisterFormat("Apache error", "%a %b %d %H:%M:%S %Y");
    RegisterFormat("ISO with offset", "%Y-%m-%dT%H:%M:%S%Ez");
}

} // namespace bench
