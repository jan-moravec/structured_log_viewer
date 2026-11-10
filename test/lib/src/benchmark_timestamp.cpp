// Benchmarks for reading timestamp text. The streaming benchmarks hide this cost, because their generated timestamps
// repeat for many lines in a row and the last parsed text is cached. Here every value is different, so each one is
// parsed. Each format runs twice: through TimestampFormat, which reads it with hand-written steps, and through
// date::parse, which every format used before the steps existed.

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"

#include <loglib/time/timestamp_parsing.hpp>

#include <benchmark/benchmark.h>
#include <date/date.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
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

} // namespace

void RegisterTimestampBenchmarks()
{
    RegisterFormat("Apache access", "%d/%b/%Y:%H:%M:%S %z");
    RegisterFormat("Apache error", "%a %b %d %H:%M:%S %Y");
    RegisterFormat("ISO with offset", "%Y-%m-%dT%H:%M:%S%Ez");
}

} // namespace bench
