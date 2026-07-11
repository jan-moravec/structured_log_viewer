#include <test_common/log_generator.hpp>

#include <date/date.h>
#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

namespace test_common
{

namespace
{

// Canonical level pool. Order is load-bearing (matched by `SlfjLevel`,
// the Java / Apache-error regex synthesizers, and the enum-promotion
// benchmarks). Sampled via `LEVEL_WEIGHTS` (geometric pyramid, `trace`
// most frequent) so fixtures see a production-ish mix. All weights are
// positive so all six aliases appear in any long corpus (relied on by
// dictionary-promotion benchmarks).
constexpr std::array<std::string_view, 6> LEVELS = {"trace", "debug", "info", "warning", "error", "fatal"};
constexpr std::array<double, 6> LEVEL_WEIGHTS = {32.0, 16.0, 8.0, 4.0, 2.0, 1.0};
constexpr std::array<std::string_view, 5> COMPONENTS = {"app", "network", "database", "ui", "system"};
constexpr std::array<std::string_view, 20> WORDS = {"lorem",       "ipsum",      "dolor",      "sit",    "amet",
                                                    "consectetur", "adipiscing", "elit",       "sed",    "do",
                                                    "eiusmod",     "tempor",     "incididunt", "ut",     "labore",
                                                    "et",          "dolore",     "magna",      "aliqua", "ut"};

std::string FormatTimestamp(std::chrono::system_clock::time_point tp)
{
    return date::format("%FT%T", date::floor<std::chrono::milliseconds>(tp));
}

std::string StampTimestamp(const TimestampPolicy &policy, std::size_t lineIndex)
{
    if (policy.baseTime)
    {
        return FormatTimestamp(*policy.baseTime + policy.interval * static_cast<std::int64_t>(lineIndex));
    }
    return FormatTimestamp(std::chrono::system_clock::now());
}

// Shared factory so both generators pick from the same weighted
// distribution. `std::discrete_distribution` is cheap enough to
// build per record and keeps call sites self-contained.
std::discrete_distribution<std::size_t> MakeLevelDist()
{
    return {LEVEL_WEIGHTS.begin(), LEVEL_WEIGHTS.end()};
}

} // namespace

std::uint32_t MakeRandomSeed()
{
    std::random_device rd;
    return rd();
}

LogRecord GenerateRandomLogRecord(std::mt19937 &rng, std::size_t lineIndex, const TimestampPolicy &timestamps)
{
    auto levelDist = MakeLevelDist();
    std::uniform_int_distribution<int> componentDist(0, static_cast<int>(COMPONENTS.size()) - 1);
    std::uniform_int_distribution<int> wordDist(0, static_cast<int>(WORDS.size()) - 1);
    std::uniform_int_distribution<int> wordsCountDist(5, 20);

    std::string message;
    const int wordCount = wordsCountDist(rng);
    for (int j = 0; j < wordCount; ++j)
    {
        if (!message.empty())
        {
            message += ' ';
        }
        message += WORDS[static_cast<std::size_t>(wordDist(rng))];
    }

    LogRecord record;
    record["timestamp"] = StampTimestamp(timestamps, lineIndex);
    record["level"] = std::string(LEVELS[levelDist(rng)]);
    record["message"] = message;
    record["thread_id"] = static_cast<std::int64_t>(lineIndex % 16);
    record["component"] = std::string(COMPONENTS[static_cast<std::size_t>(componentDist(rng))]);
    return record;
}

std::vector<LogRecord> GenerateRandomLogRecords(
    std::size_t count, std::uint32_t seed, const TimestampPolicy &timestamps
)
{
    std::vector<LogRecord> records;
    records.reserve(count);
    std::mt19937 rng(seed);
    for (std::size_t i = 0; i < count; ++i)
    {
        records.emplace_back(GenerateRandomLogRecord(rng, i, timestamps));
    }
    return records;
}

std::vector<LogRecord> GenerateRandomLogRecords(std::size_t count)
{
    return GenerateRandomLogRecords(count, MakeRandomSeed());
}

namespace
{

enum class Family
{
    String,
    Numeric,
    Boolean,
    Null,
    Array,
    Object,
};

std::vector<std::pair<std::string, Family>> BuildWideKeyList(std::size_t columnCount)
{
    static const std::array<std::string_view, 10> STRING_KEYS = {
        "timestamp", "level", "component", "message", "module", "host", "user", "session", "request", "trace_id"
    };
    static const std::array<std::string_view, 10> NUMERIC_KEYS = {
        "latency_ms",
        "bytes_in",
        "bytes_out",
        "thread_id",
        "request_id",
        "response_id",
        "queue_len",
        "retry_count",
        "memory_usage",
        "cpu_usage_pct"
    };
    static const std::array<std::string_view, 5> BOOL_KEYS = {
        "is_error", "cache_hit", "authenticated", "throttled", "secure"
    };
    static const std::array<std::string_view, 5> OTHER_KEYS = {
        "trace_data", "tags", "metadata", "extras", "annotations"
    };

    std::vector<std::pair<std::string, Family>> keys;
    keys.reserve(columnCount);

    auto pushFamily = [&](const auto &source, Family family, std::size_t take) {
        for (std::size_t i = 0; i < take; ++i)
        {
            const std::size_t bucket = i / source.size();
            const std::size_t idx = i % source.size();
            std::string keyName(source[idx]);
            if (bucket > 0)
            {
                keyName += "_";
                keyName += std::to_string(bucket);
            }
            keys.emplace_back(std::move(keyName), family);
        }
    };

    const std::size_t totalString = std::min<std::size_t>(columnCount, 10);
    const std::size_t remainingAfterString = columnCount - totalString;
    const std::size_t totalNumeric = std::min<std::size_t>(remainingAfterString, 10);
    const std::size_t remainingAfterNumeric = remainingAfterString - totalNumeric;
    const std::size_t totalBoolean = std::min<std::size_t>(remainingAfterNumeric, 5);
    std::size_t remaining = remainingAfterNumeric - totalBoolean;

    pushFamily(STRING_KEYS, Family::String, totalString);
    pushFamily(NUMERIC_KEYS, Family::Numeric, totalNumeric);
    pushFamily(BOOL_KEYS, Family::Boolean, totalBoolean);

    const std::size_t nullCount = (remaining + 2) / 3;
    remaining -= std::min(nullCount, remaining);
    const std::size_t arrayCount = (remaining + 1) / 2;
    remaining -= std::min(arrayCount, remaining);
    const std::size_t objectCount = remaining;

    pushFamily(OTHER_KEYS, Family::Null, nullCount);
    pushFamily(OTHER_KEYS, Family::Array, arrayCount);
    pushFamily(OTHER_KEYS, Family::Object, objectCount);

    while (keys.size() < columnCount)
    {
        const std::size_t shape = keys.size() % 4;
        const std::size_t bucket = keys.size() / 4;
        if (shape == 0)
        {
            keys.emplace_back("string_extra_" + std::to_string(bucket), Family::String);
        }
        else if (shape == 1)
        {
            keys.emplace_back("number_extra_" + std::to_string(bucket), Family::Numeric);
        }
        else if (shape == 2)
        {
            keys.emplace_back("bool_extra_" + std::to_string(bucket), Family::Boolean);
        }
        else
        {
            keys.emplace_back("object_extra_" + std::to_string(bucket), Family::Object);
        }
    }

    return keys;
}

} // namespace

std::vector<LogRecord> GenerateWideLogRecords(
    std::size_t count, std::size_t columnCount, std::uint32_t seed, const TimestampPolicy &timestamps
)
{
    const auto keys = BuildWideKeyList(columnCount);

    std::vector<LogRecord> records;
    records.reserve(count);

    std::mt19937 rng(seed);
    auto levelDist = MakeLevelDist();
    std::uniform_int_distribution<int> componentDist(0, static_cast<int>(COMPONENTS.size()) - 1);
    std::uniform_int_distribution<int> wordDist(0, static_cast<int>(WORDS.size()) - 1);
    std::uniform_int_distribution<int> wordsCountDist(3, 8);
    std::uniform_int_distribution<int> intDist(0, 1'000'000);
    std::uniform_int_distribution<int> smallIntDist(0, 100);

    for (std::size_t i = 0; i < count; ++i)
    {
        LogRecord json;
        for (std::size_t k = 0; k < keys.size(); ++k)
        {
            const std::string &keyName = keys[k].first;
            switch (keys[k].second)
            {
            case Family::String:
            {
                if (keyName.starts_with("timestamp"))
                {
                    json[keyName] = StampTimestamp(timestamps, i);
                }
                else if (keyName.starts_with("level"))
                {
                    json[keyName] = std::string(LEVELS[levelDist(rng)]);
                }
                else if (keyName.starts_with("component"))
                {
                    json[keyName] = std::string(COMPONENTS[static_cast<std::size_t>(componentDist(rng))]);
                }
                else
                {
                    std::string value;
                    const int wc = wordsCountDist(rng);
                    for (int j = 0; j < wc; ++j)
                    {
                        if (!value.empty())
                        {
                            value += ' ';
                        }
                        value += WORDS[static_cast<std::size_t>(wordDist(rng))];
                    }
                    json[keyName] = value;
                }
                break;
            }
            case Family::Numeric:
            {
                if (keyName.starts_with("thread_id"))
                {
                    json[keyName] = static_cast<std::int64_t>(i % 16);
                }
                else if (keyName.starts_with("cpu_usage_pct"))
                {
                    json[keyName] = static_cast<std::int64_t>(smallIntDist(rng));
                }
                else
                {
                    json[keyName] = static_cast<std::int64_t>(intDist(rng));
                }
                break;
            }
            case Family::Boolean:
            {
                json[keyName] = ((i + k) & 1) == 0;
                break;
            }
            case Family::Null:
            {
                json[keyName] = nullptr;
                break;
            }
            case Family::Array:
            {
                std::vector<glz::generic_sorted_u64> arr;
                arr.emplace_back(static_cast<std::int64_t>(intDist(rng)));
                arr.emplace_back(static_cast<std::int64_t>(smallIntDist(rng)));
                arr.emplace_back(std::string(WORDS[static_cast<std::size_t>(wordDist(rng))]));
                json[keyName] = arr;
                break;
            }
            case Family::Object:
            {
                glz::generic_sorted_u64 obj;
                obj["k"] = static_cast<std::int64_t>(smallIntDist(rng));
                obj["v"] = std::string(WORDS[static_cast<std::size_t>(wordDist(rng))]);
                json[keyName] = std::move(obj);
                break;
            }
            }
        }
        records.emplace_back(std::move(json));
    }

    return records;
}

std::vector<LogRecord> GenerateWideLogRecords(std::size_t count, std::size_t columnCount)
{
    return GenerateWideLogRecords(count, columnCount, MakeRandomSeed());
}

} // namespace test_common
