#include "common.hpp"

#include "buffering_sink.hpp"

#include <loglib/internal/parser_options.hpp>
#include <loglib/json_parser.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_factory.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/log_table.hpp>
#include <loglib/parser_options.hpp>

#include <catch2/catch_all.hpp>
#include <date/date.h>
#include <glaze/glaze.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <numeric>
#include <random>
#include <stop_token>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace loglib;

namespace
{

// Drives a synchronous parse against the streaming pipeline (mirroring the legacy
// `JsonParser::Parse(path, opts)` shape) for benchmarks that need to dial advanced
// tuning knobs while still consuming a `ParseResult`. The default-options
// `parser.Parse(path)` overload routes through the same `BufferingSink` plumbing.
ParseResult ParseWithSink(
    const LogParser &parser,
    const std::filesystem::path &path,
    const ParserOptions &options = {},
    const internal::AdvancedParserOptions &advanced = {}
)
{
    auto logFile = std::make_unique<LogFile>(path);
    LogFile *logFilePtr = logFile.get();
    BufferingSink sink(std::move(logFile));
    parser.ParseStreaming(*logFilePtr, sink, options, advanced);
    LogData data = sink.TakeData();
    std::vector<std::string> errors = sink.TakeErrors();
    return ParseResult{std::move(data), std::move(errors)};
}

// Convenience helper that prints the throughput numbers for a parse-style
// benchmark via Catch2's WARN macro. We compute MB/s of input parsed and
// lines/s separately so PR descriptions can quote both (PRD req. 4.5.34).
// WARN is used (rather than INFO) so the line shows up in the test log on
// success; functionally this is just a report.
void ReportThroughput(const char *label, std::chrono::nanoseconds elapsed, size_t bytes, size_t lines)
{
    if (elapsed.count() == 0)
    {
        return;
    }
    const double seconds = std::chrono::duration<double>(elapsed).count();
    const double mbps = (static_cast<double>(bytes) / (1024.0 * 1024.0)) / seconds;
    const double linesPerSec = static_cast<double>(lines) / seconds;
    WARN(
        label << ": " << mbps << " MB/s, " << linesPerSec << " lines/s (" << bytes << " bytes / " << lines
              << " lines / " << seconds << "s)"
    );
}

// PRD §7.4 / G4 — manual replacement for Catch2's `BENCHMARK_ADVANCED` on the
// long-running parse fixtures. `BENCHMARK_ADVANCED` runs an iteration-
// estimation pass (calling the lambda 1, 2, 4, … times until elapsed ≥ a
// threshold) and a 100-resample bootstrap analysis on top of the user-
// requested samples. For a 1-second-per-call benchmark like `[large]` /
// `[wide]` / `[stream_to_table]` that adds 3-5× to total wall-time per run
// and made `[stream_to_table]` time out at 30 minutes during PRD task 1.0
// baseline capture. This helper just runs the lambda `samples` times,
// timings each via `steady_clock`, and emits a one-line WARN with mean /
// low / high / stddev so the per-commit numbers stay copy-pasteable into
// PR descriptions.
//
// The number of samples is intentionally small (5 by default — same as
// what the `--benchmark-samples 5` flag selected for these fixtures during
// task 1.0). Increase via the `samples` argument at call sites that need
// tighter confidence intervals; on a 1-s benchmark a 5-sample run already
// surfaces single-digit-percent regressions reliably (G4's ±3 % gate).
template <typename Fn>
void RunTimedSamples(const char *label, std::size_t samples, Fn &&fn)
{
    REQUIRE(samples > 0);
    std::vector<std::chrono::nanoseconds> elapsed;
    elapsed.reserve(samples);
    for (std::size_t i = 0; i < samples; ++i)
    {
        const auto start = std::chrono::steady_clock::now();
        fn();
        elapsed.push_back(std::chrono::steady_clock::now() - start);
    }

    const auto sum = std::accumulate(elapsed.begin(), elapsed.end(), std::chrono::nanoseconds::zero());
    const auto mean = sum / static_cast<long long>(samples);
    const auto low = *std::min_element(elapsed.begin(), elapsed.end());
    const auto high = *std::max_element(elapsed.begin(), elapsed.end());

    double meanNs = static_cast<double>(mean.count());
    double sqAccum = 0.0;
    for (const auto &e : elapsed)
    {
        const double d = static_cast<double>(e.count()) - meanNs;
        sqAccum += d * d;
    }
    const double stddevNs = std::sqrt(sqAccum / static_cast<double>(samples));

    using ms = std::chrono::duration<double, std::milli>;
    WARN(
        label << " (samples=" << samples << "): mean=" << ms(mean).count() << " ms, low=" << ms(low).count()
              << " ms, high=" << ms(high).count() << " ms, stddev=" << (stddevNs / 1'000'000.0) << " ms"
    );
}

} // namespace

// Helper function to generate random JSON log entries
std::vector<TestJsonLogFile::Line> GenerateRandomJsonLogs(size_t count)
{
    std::vector<TestJsonLogFile::Line> logs;
    logs.reserve(count);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> level_dist(0, 3);
    std::uniform_int_distribution<> component_dist(0, 4);
    std::uniform_int_distribution<> words_count_dist(5, 20); // Random number of words per message

    static const std::array<std::string, 6> LEVELS = {"trace", "debug", "info", "warning", "error", "fatal"};
    static const std::array<std::string, 5> COMPONENTS = {"app", "network", "database", "ui", "system"};
    static const std::array<std::string, 20> WORDS = {"lorem",       "ipsum",      "dolor",      "sit",    "amet",
                                                      "consectetur", "adipiscing", "elit",       "sed",    "do",
                                                      "eiusmod",     "tempor",     "incididunt", "ut",     "labore",
                                                      "et",          "dolore",     "magna",      "aliqua", "ut"};

    for (size_t i = 0; i < count; ++i)
    {
        // Generate a random message from words
        std::string message;
        int word_count = words_count_dist(gen);
        for (int j = 0; j < word_count; ++j)
        {
            std::uniform_int_distribution<> word_dist(0, static_cast<int>(WORDS.size() - 1));
            if (!message.empty())
            {
                message += " ";
            }
            message += WORDS[word_dist(gen)];
        }

        glz::generic_sorted_u64 json;
        json["timestamp"] =
            date::format("%FT%T", date::floor<std::chrono::milliseconds>(std::chrono::system_clock::now()));
        json["level"] = LEVELS[level_dist(gen)];
        json["message"] = message;
        json["thread_id"] = i % 16;
        json["component"] = COMPONENTS[component_dist(gen)];
        logs.emplace_back(json);
    }

    return logs;
}

// PRD §4.7.3 / parser-perf task 8.8 — wide-row fixture generator.
//
// Stresses the per-line field-iteration cost (`InsertSorted`, `ExtractFieldKey`,
// `ParseLine`) and the `IsKeyInAnyColumn` cache by emitting `columnCount` keys
// per line in a fixed order. The default mix matches §4.7.3:
//   - ~10 string keys (lorem-style messages, levels, components),
//   - ~10 numeric keys (latencies, byte counts, ids),
//   - ~5 boolean keys (flags),
//   - ~5 keys that rotate through `null` / array / object so the value-shape
//     dispatch in `ParseLine` exercises every leaf.
//
// Above `columnCount = 30` the helper round-robins through the same key family
// suffixed with the column index so the line shape stays semantically wide
// without inventing arbitrary key vocabularies. Below 30, we trim the longest
// family last (strings > numbers > booleans > others) so the proportions stay
// close to the §4.7.3 mix even at 10–20 columns.
//
// Note: every line emits the keys in the **same** fixed iteration order. That
// makes the per-worker `ParseCache` and `KeyIndex` warm-up paths deterministic
// across runs, so the `[wide]` numbers are repeatable enough to use as a
// regression gate (PRD req. 4.7.5).
std::vector<TestJsonLogFile::Line> GenerateWideJsonLogs(size_t count, size_t columnCount = 30)
{
    static const std::array<std::string, 10> STRING_KEYS = {"timestamp", "level", "component", "message", "module",
                                                            "host",      "user",  "session",   "request", "trace_id"};
    static const std::array<std::string, 10> NUMERIC_KEYS = {"latency_ms",   "bytes_in",     "bytes_out", "thread_id",
                                                             "request_id",   "response_id",  "queue_len", "retry_count",
                                                             "memory_usage", "cpu_usage_pct"};
    static const std::array<std::string, 5> BOOL_KEYS = {"is_error", "cache_hit", "authenticated", "throttled", "secure"};
    static const std::array<std::string, 5> OTHER_KEYS = {"trace_data", "tags", "metadata", "extras", "annotations"};

    static const std::array<std::string, 6> LEVELS = {"trace", "debug", "info", "warning", "error", "fatal"};
    static const std::array<std::string, 5> COMPONENTS = {"app", "network", "database", "ui", "system"};
    static const std::array<std::string, 20> WORDS = {"lorem",       "ipsum",      "dolor",      "sit",    "amet",
                                                      "consectetur", "adipiscing", "elit",       "sed",    "do",
                                                      "eiusmod",     "tempor",     "incididunt", "ut",     "labore",
                                                      "et",          "dolore",     "magna",      "aliqua", "ut"};

    // Build the deterministic key list once. Each entry tags the family so we
    // know what value shape to emit per row.
    enum class Family
    {
        String,
        Numeric,
        Boolean,
        Null,
        Array,
        Object,
    };
    std::vector<std::pair<std::string, Family>> keys;
    keys.reserve(columnCount);

    auto pushFamily = [&](const auto &source, Family family, size_t take) {
        for (size_t i = 0; i < take; ++i)
        {
            const size_t bucket = i / source.size();
            const size_t idx = i % source.size();
            std::string keyName = source[idx];
            if (bucket > 0)
            {
                keyName += "_";
                keyName += std::to_string(bucket);
            }
            keys.emplace_back(std::move(keyName), family);
        }
    };

    // Trim from "others" first so the §4.7.3 string/number proportions stay
    // intact when the caller picks `columnCount < 30`.
    const size_t totalString = std::min<size_t>(columnCount, 10);
    const size_t remainingAfterString = columnCount - totalString;
    const size_t totalNumeric = std::min<size_t>(remainingAfterString, 10);
    const size_t remainingAfterNumeric = remainingAfterString - totalNumeric;
    const size_t totalBoolean = std::min<size_t>(remainingAfterNumeric, 5);
    size_t remaining = remainingAfterNumeric - totalBoolean;

    pushFamily(STRING_KEYS, Family::String, totalString);
    pushFamily(NUMERIC_KEYS, Family::Numeric, totalNumeric);
    pushFamily(BOOL_KEYS, Family::Boolean, totalBoolean);

    // The remainder rotates between null / array / object. We split it as
    // evenly as possible so each leaf shape runs at every line.
    const size_t nullCount = (remaining + 2) / 3;
    remaining -= std::min(nullCount, remaining);
    const size_t arrayCount = (remaining + 1) / 2;
    remaining -= std::min(arrayCount, remaining);
    const size_t objectCount = remaining;

    pushFamily(OTHER_KEYS, Family::Null, nullCount);
    pushFamily(OTHER_KEYS, Family::Array, arrayCount);
    pushFamily(OTHER_KEYS, Family::Object, objectCount);

    // Round-robin pad if columnCount > the natural cap. We keep the shape mix
    // proportional by cycling through families in the same string/number/bool/
    // other ratio used above.
    while (keys.size() < columnCount)
    {
        const size_t shape = keys.size() % 4;
        const size_t bucket = keys.size() / 4;
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

    std::vector<TestJsonLogFile::Line> logs;
    logs.reserve(count);

    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> level_dist(0, static_cast<int>(LEVELS.size()) - 1);
    std::uniform_int_distribution<> component_dist(0, static_cast<int>(COMPONENTS.size()) - 1);
    std::uniform_int_distribution<> word_dist(0, static_cast<int>(WORDS.size()) - 1);
    std::uniform_int_distribution<> words_count_dist(3, 8);
    std::uniform_int_distribution<int> int_dist(0, 1'000'000);
    std::uniform_int_distribution<int> small_int_dist(0, 100);

    for (size_t i = 0; i < count; ++i)
    {
        glz::generic_sorted_u64 json;
        for (size_t k = 0; k < keys.size(); ++k)
        {
            const std::string &keyName = keys[k].first;
            switch (keys[k].second)
            {
            case Family::String: {
                if (keyName.rfind("timestamp", 0) == 0)
                {
                    json[keyName] = date::format(
                        "%FT%T", date::floor<std::chrono::milliseconds>(std::chrono::system_clock::now())
                    );
                }
                else if (keyName.rfind("level", 0) == 0)
                {
                    json[keyName] = LEVELS[level_dist(gen)];
                }
                else if (keyName.rfind("component", 0) == 0)
                {
                    json[keyName] = COMPONENTS[component_dist(gen)];
                }
                else
                {
                    std::string value;
                    const int wc = words_count_dist(gen);
                    for (int j = 0; j < wc; ++j)
                    {
                        if (!value.empty())
                        {
                            value += " ";
                        }
                        value += WORDS[word_dist(gen)];
                    }
                    json[keyName] = value;
                }
                break;
            }
            case Family::Numeric: {
                if (keyName.rfind("thread_id", 0) == 0)
                {
                    json[keyName] = static_cast<int64_t>(i % 16);
                }
                else if (keyName.rfind("cpu_usage_pct", 0) == 0)
                {
                    json[keyName] = static_cast<int64_t>(small_int_dist(gen));
                }
                else
                {
                    json[keyName] = static_cast<int64_t>(int_dist(gen));
                }
                break;
            }
            case Family::Boolean: {
                json[keyName] = ((i + k) & 1) == 0;
                break;
            }
            case Family::Null: {
                json[keyName] = nullptr;
                break;
            }
            case Family::Array: {
                std::vector<glz::generic_sorted_u64> arr;
                arr.emplace_back(static_cast<int64_t>(int_dist(gen)));
                arr.emplace_back(static_cast<int64_t>(small_int_dist(gen)));
                arr.emplace_back(WORDS[word_dist(gen)]);
                json[keyName] = std::move(arr);
                break;
            }
            case Family::Object: {
                glz::generic_sorted_u64 obj;
                obj["k"] = static_cast<int64_t>(small_int_dist(gen));
                obj["v"] = WORDS[word_dist(gen)];
                json[keyName] = std::move(obj);
                break;
            }
            }
        }
        logs.emplace_back(std::move(json));
    }

    return logs;
}

TEST_CASE("Parse and load JSON log", "[.][benchmark][json_parser]")
{
    // 1.7 MB per 10'000 lines of data
    auto logs = GenerateRandomJsonLogs(10'000);
    const TestJsonLogFile testFile(logs);
    const JsonParser parser;
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    {
        const auto start = std::chrono::steady_clock::now();
        ParseResult warmup = parser.Parse(testFile.GetFilePath());
        const auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(warmup.data.Lines().size() == logs.size());
        REQUIRE(warmup.errors.empty());
        ReportThroughput("Parse 10'000 warm-up", elapsed, bytes, logs.size());
    }

    RunTimedSamples("Parse 10'000 JSON log entries", 5, [&]() {
        LogTable table;
        ParseResult result = parser.Parse(testFile.GetFilePath());
        REQUIRE(result.data.Lines().size() == testFile.Lines().size());
        REQUIRE(result.errors.empty());
        table.Update(std::move(result.data));
    });
}

TEST_CASE("Parse and load JSON log (single thread)", "[.][benchmark][json_parser][single_thread]")
{
    // Forces the streaming pipeline down to one Stage B worker so we can compare against the
    // default-parallelism benchmark and quantify oneTBB's speedup (PRD req. 4.5.34). Same fixture
    // size as the default benchmark to keep the numbers directly comparable.
    auto logs = GenerateRandomJsonLogs(10'000);
    const TestJsonLogFile testFile(logs);
    const JsonParser parser;
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    ParserOptions opts;
    internal::AdvancedParserOptions advanced;
    advanced.threads = 1;

    {
        const auto start = std::chrono::steady_clock::now();
        ParseResult warmup = ParseWithSink(parser, testFile.GetFilePath(), opts, advanced);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(warmup.data.Lines().size() == logs.size());
        REQUIRE(warmup.errors.empty());
        ReportThroughput("Parse 10'000 (1 thread) warm-up", elapsed, bytes, logs.size());
    }

    RunTimedSamples("Parse 10'000 JSON log entries (single thread)", 5, [&]() {
        LogTable table;
        ParseResult result = ParseWithSink(parser, testFile.GetFilePath(), opts, advanced);
        REQUIRE(result.data.Lines().size() == testFile.Lines().size());
        REQUIRE(result.errors.empty());
        table.Update(std::move(result.data));
    });
}

namespace
{

// PRD §4.7.2 / parser-perf task 8.10 — per-stage breakdown emit.
//
// Mirrors the §4.7.2 reference format verbatim:
//   "Wall-clock: 1.55 s | Stage A CPU: 80 ms | Stage B CPU: 11.2 s
//    (across 8 workers, 90 % utilisation = 11.2 / (8 × 1.55)) |
//    Stage C CPU: 95 ms | Sink: 110 ms"
// Stage B utilisation is derived as
//   `stageBCpuTotal / (effectiveThreads × wallClockTotal)`.
void ReportStageTimings(const char *label, const StageTimings &t)
{
    using ms = std::chrono::duration<double, std::milli>;
    using s = std::chrono::duration<double>;
    const double wallClockS = s(t.wallClockTotal).count();
    const double stageAMs = ms(t.stageACpuTotal).count();
    const double stageBS = s(t.stageBCpuTotal).count();
    const double stageCMs = ms(t.stageCCpuTotal).count();
    const double sinkMs = ms(t.sinkTotal).count();
    const double denom = static_cast<double>(t.effectiveThreads) * wallClockS;
    const double utilization = denom == 0.0 ? 0.0 : (stageBS / denom) * 100.0;
    WARN(
        label << " — Wall-clock: " << wallClockS << " s | Stage A CPU: " << stageAMs
              << " ms | Stage B CPU: " << stageBS << " s (across " << t.effectiveThreads << " workers, "
              << utilization << " % utilisation = " << stageBS << " / (" << t.effectiveThreads << " * " << wallClockS
              << ")) | Stage C CPU: " << stageCMs << " ms | Sink: " << sinkMs
              << " ms | batches A/B/C=" << t.stageABatches << "/" << t.stageBBatches << "/" << t.stageCBatches
    );
}

// PRD §4.7.5 / parser-perf task 8.11 — no-regression checks.
//
// Every commit on this branch must include before/after MB/s for the three
// regression-gating fixtures `[large]`, `[wide]`, `[stream_to_table]` in its
// commit message. The PR/CI checklist must also include:
//   - `[allocations]` fast-path fraction ≥ 99 %  (M5)
//   - Stage B utilisation > 70 %                  (M4)
//   - Stage A wall-clock %  < 5 %                 (M4)
//   - `[cancellation]` p95 latency within ±3 % of the prior commit's number
//
// The numbers come from the `WARN` lines emitted by `ReportThroughput` above
// and `ReportStageTimings` (per task 8.10).
//
// The PRD's §7.4 acceptance gate G4 is "±3 % per change OR a documented
// architectural justification in the commit message". This comment block is
// the authoritative reference for what every commit-message PR-block must
// quote (so a future contributor knows what to copy in).
//
// NOTE: keep the comment list in sync with the WARN format if `[wide]` or
// `[stream_to_table]` later add fields — the format is the source of truth
// for the gate.

} // namespace

// PRD task 6.1 — large-file benchmark (1'000'000 lines, ~170 MB). Tagged
// `[large]` so it is opt-in via Catch2's tag filter and does not slow the
// default `[benchmark]` run.
TEST_CASE("Parse and load JSON log (1'000'000 lines)", "[.][benchmark][json_parser][large]")
{
    auto logs = GenerateRandomJsonLogs(1'000'000);
    const TestJsonLogFile testFile(logs);
    const JsonParser parser;
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    // PRD task 6.6 — capture wall-clock for one untimed warm-up run so the
    // throughput numbers (MB/s, lines/s) end up in the test log alongside
    // the per-stage breakdown emitted by `ReportStageTimings`. The timed-
    // sample loop below (`RunTimedSamples`) only emits ns/sample, so the
    // MB/s + per-stage numbers come from this warm-up.
    {
        StageTimings timings;
        ParserOptions warmupOpts;
        internal::AdvancedParserOptions warmupAdvanced;
        warmupAdvanced.timings = &timings;

        const auto start = std::chrono::steady_clock::now();
        ParseResult warmup = ParseWithSink(parser, testFile.GetFilePath(), warmupOpts, warmupAdvanced);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(warmup.data.Lines().size() == logs.size());
        REQUIRE(warmup.errors.empty());
        ReportThroughput("Parse 1'000'000 warm-up", elapsed, bytes, logs.size());
        ReportStageTimings("Parse 1'000'000 warm-up", timings);
    }

    RunTimedSamples("Parse 1'000'000 JSON log entries", 5, [&]() {
        LogTable table;
        ParseResult result = parser.Parse(testFile.GetFilePath());
        REQUIRE(result.data.Lines().size() == logs.size());
        REQUIRE(result.errors.empty());
        table.Update(std::move(result.data));
    });
}

// PRD §4.7.4 / parser-perf task 8.9 — wide-row benchmark (1'000'000 lines,
// ~30 fields per line). Stresses the per-line field-iteration cost
// (`InsertSorted`, `ExtractFieldKey`, `ParseLine`) and the
// `IsKeyInAnyColumn` cache from §4.7.6 against a configuration that has
// every emitted key registered up front. Reports the same MB/s / lines/s /
// per-stage breakdown as `[large]` so the two are directly comparable.
TEST_CASE("Parse and load JSON log (wide, 1'000'000 lines)", "[.][benchmark][json_parser][wide]")
{
    auto logs = GenerateWideJsonLogs(1'000'000);
    const TestJsonLogFile testFile(logs);
    const JsonParser parser;
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    // Untimed warm-up + per-stage breakdown emit, mirroring `[large]`.
    {
        StageTimings timings;
        ParserOptions warmupOpts;
        internal::AdvancedParserOptions warmupAdvanced;
        warmupAdvanced.timings = &timings;

        const auto start = std::chrono::steady_clock::now();
        ParseResult warmup = ParseWithSink(parser, testFile.GetFilePath(), warmupOpts, warmupAdvanced);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(warmup.data.Lines().size() == logs.size());
        REQUIRE(warmup.errors.empty());
        ReportThroughput("Parse wide warm-up", elapsed, bytes, logs.size());
        ReportStageTimings("Parse wide warm-up", timings);

        // Fast-path fraction reporting (M5). Walks the parsed values once and
        // counts string_view (fast) vs. owned string (slow) hits per the same
        // recipe as the `[allocations]` benchmark. Enforced ≥ 99 % via the
        // commit gate, not the test itself, so a regression in this number
        // surfaces in the PR description rather than as a hard failure.
        size_t totalStringValues = 0;
        size_t stringViewValues = 0;
        for (const LogLine &line : warmup.data.Lines())
        {
            for (size_t i = 0; i < warmup.data.Keys().Size(); ++i)
            {
                const LogValue &v = line.GetValue(static_cast<KeyId>(i));
                if (std::holds_alternative<std::string_view>(v))
                {
                    ++stringViewValues;
                    ++totalStringValues;
                }
                else if (std::holds_alternative<std::string>(v))
                {
                    ++totalStringValues;
                }
            }
        }
        const double fastPathFraction = totalStringValues == 0
                                            ? 0.0
                                            : static_cast<double>(stringViewValues) /
                                                  static_cast<double>(totalStringValues);
        WARN(
            "Parse wide warm-up — string_view fast-path fraction: " << (fastPathFraction * 100.0) << "% ("
                                                                    << stringViewValues << " / " << totalStringValues
                                                                    << ")"
        );
    }

    RunTimedSamples("Parse 1'000'000 wide JSON log entries", 5, [&]() {
        LogTable table;
        ParseResult result = parser.Parse(testFile.GetFilePath());
        REQUIRE(result.data.Lines().size() == logs.size());
        REQUIRE(result.errors.empty());
        table.Update(std::move(result.data));
    });
}

// PRD task 6.2 — `useThreadLocalKeyCache=false` variant. Lets us bisect how
// much of the multi-threaded speedup comes from the per-worker interned key
// cache vs. the simdjson + KeyIndex hot path itself (PRD req. 4.1.2/2b).
TEST_CASE(
    "Parse and load JSON log (no thread-local key cache)", "[.][benchmark][json_parser][no_thread_local_cache]"
)
{
    auto logs = GenerateRandomJsonLogs(10'000);
    const TestJsonLogFile testFile(logs);
    const JsonParser parser;
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    ParserOptions opts;
    internal::AdvancedParserOptions advanced;
    advanced.useThreadLocalKeyCache = false;

    {
        const auto start = std::chrono::steady_clock::now();
        ParseResult warmup = ParseWithSink(parser, testFile.GetFilePath(), opts, advanced);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(warmup.data.Lines().size() == logs.size());
        REQUIRE(warmup.errors.empty());
        ReportThroughput("Parse 10'000 (no thread-local key cache) warm-up", elapsed, bytes, logs.size());
    }

    RunTimedSamples("Parse 10'000 JSON log entries (no thread-local key cache)", 5, [&]() {
        LogTable table;
        ParseResult result = ParseWithSink(parser, testFile.GetFilePath(), opts, advanced);
        REQUIRE(result.data.Lines().size() == testFile.Lines().size());
        REQUIRE(result.errors.empty());
        table.Update(std::move(result.data));
    });
}

// PRD task 6.3 — `useParseCache=false` variant. Disables the per-worker
// simdjson type-cache so we can quantify the savings from skipping the
// per-field `value.type()` round-trip (PRD req. 4.1.15).
TEST_CASE("Parse and load JSON log (no parse cache)", "[.][benchmark][json_parser][no_parse_cache]")
{
    auto logs = GenerateRandomJsonLogs(10'000);
    const TestJsonLogFile testFile(logs);
    const JsonParser parser;
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    ParserOptions opts;
    internal::AdvancedParserOptions advanced;
    advanced.useParseCache = false;

    {
        const auto start = std::chrono::steady_clock::now();
        ParseResult warmup = ParseWithSink(parser, testFile.GetFilePath(), opts, advanced);
        const auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(warmup.data.Lines().size() == logs.size());
        REQUIRE(warmup.errors.empty());
        ReportThroughput("Parse 10'000 (no parse cache) warm-up", elapsed, bytes, logs.size());
    }

    RunTimedSamples("Parse 10'000 JSON log entries (no parse cache)", 5, [&]() {
        LogTable table;
        ParseResult result = ParseWithSink(parser, testFile.GetFilePath(), opts, advanced);
        REQUIRE(result.data.Lines().size() == testFile.Lines().size());
        REQUIRE(result.errors.empty());
        table.Update(std::move(result.data));
    });
}

// PRD task 6.5 — `LogLine::GetValue` micro-benchmark. Walks every field of
// every parsed line via the slow path (key string -> KeyIndex -> KeyId) and
// then via the fast path (cached KeyId) so the README can quote the speedup
// PRD req. 4.1.10/4.1.10a calls out.
TEST_CASE("LogLine::GetValue micro-benchmark", "[.][benchmark][log_line][get_value_micro]")
{
    auto logs = GenerateRandomJsonLogs(10'000);
    const TestJsonLogFile testFile(logs);
    const JsonParser parser;

    // Pre-parse once so the benchmark only measures the lookup cost.
    ParseResult result = parser.Parse(testFile.GetFilePath());
    REQUIRE(result.errors.empty());
    const LogData &data = result.data;
    const std::vector<LogLine> &lines = data.Lines();
    REQUIRE(!lines.empty());

    // Snapshot the keys we will look up. Use the canonical KeyIndex's known
    // field names so the slow path actually has work to do.
    const std::array<std::string, 5> kKeys = {"timestamp", "level", "message", "thread_id", "component"};
    std::array<KeyId, 5> keyIds{};
    for (size_t i = 0; i < kKeys.size(); ++i)
    {
        const KeyId id = data.Keys().Find(kKeys[i]);
        REQUIRE(id != kInvalidKeyId);
        keyIds[i] = id;
    }

    // Use `volatile` to force the optimiser to keep the lookup in the loop
    // body rather than fold the whole walk away (the previous Catch2
    // `BENCHMARK` macro had a built-in `Catch::Benchmark::keep_memory()` /
    // result-return guard; `RunTimedSamples` does not, so we need our own).
    volatile size_t hitsSink = 0;

    RunTimedSamples("LogLine::GetValue(string) — slow path", 11, [&]() {
        size_t hits = 0;
        for (const LogLine &line : lines)
        {
            for (const std::string &key : kKeys)
            {
                if (!std::holds_alternative<std::monostate>(line.GetValue(key)))
                {
                    ++hits;
                }
            }
        }
        hitsSink = hits;
    });

    RunTimedSamples("LogLine::GetValue(KeyId) — fast path", 11, [&]() {
        size_t hits = 0;
        for (const LogLine &line : lines)
        {
            for (const KeyId id : keyIds)
            {
                if (!std::holds_alternative<std::monostate>(line.GetValue(id)))
                {
                    ++hits;
                }
            }
        }
        hitsSink = hits;
    });

    (void)hitsSink;
}

// PRD tasks 6.4 + 6.8 — structural allocation / fast-path fraction report.
//
// We deliberately do **not** override global `operator new` here. A
// thread-local-gated override would work in principle, but it interacts
// awkwardly with Catch2's own allocator-heavy reporting and with TBB's worker
// threads that allocate independently of the main thread. Instead we count the
// observable structural cost of a parse:
//
//   - number of `LogLine`s (one heap-allocated vector backing per line);
//   - number of `LogValue`s holding `std::string` (one owned-string allocation
//     per value, the only per-value allocation path);
//   - number of `LogValue`s holding `std::string_view` (the fast path that
//     points back into the mmap and allocates nothing — PRD req. 4.1.15a).
//
// Together these give a tight upper bound on per-parse heap allocations
// (`#lines + #ownedStrings + O(KeyIndex growth)`). Combined with the
// fast-path fraction this satisfies both PRD bullet points:
//   * 6.4: "demonstrate that the per-line allocation count after warm-up is
//     bounded by a small constant — target <= 1 per line for the owned-string
//     fallback path; 0 in the all-string_view case".
//   * 6.8: "report the fraction of string values that landed in the
//     `string_view` fast path".
TEST_CASE("Allocation footprint and string_view fast-path fraction", "[.][benchmark][json_parser][allocations]")
{
    // 1'000-line synthetic input with short ASCII string values, per the PRD
    // recipe for the [allocations] variant.
    auto logs = GenerateRandomJsonLogs(1'000);
    const TestJsonLogFile testFile(logs);
    const JsonParser parser;

    ParseResult result = parser.Parse(testFile.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == logs.size());

    size_t lineCount = result.data.Lines().size();
    size_t totalValues = 0;
    size_t stringViewValues = 0;
    size_t ownedStringValues = 0;
    for (const LogLine &line : result.data.Lines())
    {
        for (size_t i = 0; i < result.data.Keys().Size(); ++i)
        {
            const LogValue &v = line.GetValue(static_cast<KeyId>(i));
            if (std::holds_alternative<std::monostate>(v))
            {
                continue;
            }
            ++totalValues;
            if (std::holds_alternative<std::string_view>(v))
            {
                ++stringViewValues;
            }
            else if (std::holds_alternative<std::string>(v))
            {
                ++ownedStringValues;
            }
        }
    }

    const size_t totalStringValues = stringViewValues + ownedStringValues;
    const double fastPathFraction =
        totalStringValues == 0 ? 0.0 : static_cast<double>(stringViewValues) / static_cast<double>(totalStringValues);

    // Upper bound: at most one owned-string allocation per value plus one
    // backing-vector allocation per line. The KeyIndex grows by O(distinctKeys)
    // which is ~5 here and is therefore dwarfed by the per-line term.
    const size_t allocUpperBound = lineCount + ownedStringValues;

    WARN(
        "Allocation footprint over " << lineCount << " lines: " << totalValues << " values, " << stringViewValues
                                     << " string_view (fast path), " << ownedStringValues
                                     << " std::string (slow path), fast-path fraction="
                                     << (fastPathFraction * 100.0) << "%, allocation upper bound=" << allocUpperBound
                                     << " (~" << (static_cast<double>(allocUpperBound) / static_cast<double>(lineCount))
                                     << "/line)"
    );

    // Hard contract: the streaming parser must not regress to an all-owned-
    // string regime. We accept some owned strings (e.g. when simdjson reports
    // an unstable value buffer) but never zero fast-path hits on this fixture.
    REQUIRE(stringViewValues > 0);
}

// Parser-perf task 3.10 — end-to-end MainWindow-flow benchmark for PRD §4.2a /
// req. 4.2a.7 / metric M7. Mirrors the `[large]` fixture in size and content but
// drives the full streaming GUI flow: `LogTable::BeginStreaming` + a custom sink
// that calls `LogTable::AppendBatch` per `OnBatch`. Reports end-to-end MB/s plus
// the cumulative wall-time spent in the GUI-thread mid-stream timestamp back-fill
// (`LogTable::AppendBatch` step 4) so we can quantify the M7 expectation that
// Stage B's in-pipeline promotion drops the back-fill cost by ≥ 95 %.
//
// The fixture configures a `Type::time` column for `timestamp` so Stage B has
// real promotion work to do — without it the back-fill loop stays a no-op for
// every batch and the benchmark would not exercise M7 at all.
TEST_CASE(
    "Parse and stream to LogTable (1'000'000 lines)", "[.][benchmark][json_parser][stream_to_table]"
)
{
    auto logs = GenerateRandomJsonLogs(1'000'000);
    const TestJsonLogFile testFile(logs);
    const JsonParser parser;
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    InitializeTimezoneData();

    // Configuration mirrors the GUI's typical timestamp column shape exactly —
    // `LogTable::BeginStreaming`'s snapshot will pick up this column up front, so
    // Stage B promotes every line's `timestamp` value inline and the GUI-thread
    // mid-stream back-fill loop only fires for *additional* time columns auto-
    // promoted from keys first observed in a later batch (none in this fixture).
    LogConfiguration baseConfig;
    LogConfiguration::Column timestampColumn;
    timestampColumn.header = "timestamp";
    timestampColumn.keys = {"timestamp"};
    timestampColumn.type = LogConfiguration::Type::time;
    timestampColumn.parseFormats = {"%FT%T"};
    timestampColumn.printFormat = "%F %H:%M:%S";
    baseConfig.columns.push_back(std::move(timestampColumn));
    auto configuration = std::make_shared<LogConfiguration>(baseConfig);

    // Persist the configuration to a temp JSON file so we can install it into the
    // table's `LogConfigurationManager` via `Load`. The manager has no public
    // setter for a fully-formed LogConfiguration; `Load` is the canonical entry
    // point used by the GUI as well.
    TestLogConfiguration configFile;
    configFile.Write(baseConfig);

    // Sink that mirrors `LogModel::OnBatch`: route every Stage C batch into
    // `LogTable::AppendBatch` (which runs the GUI-thread back-fill loop step 4).
    // We instrument the back-fill step by re-running it inline ourselves before
    // calling AppendBatch — but that would double-fill. Instead, we time the
    // AppendBatch call itself (which includes the back-fill loop as its hottest
    // sub-step in the legacy path) and rely on the M7 expectation that Stage B
    // promotion makes the overwhelming majority of that time disappear.
    struct StreamSink : StreamingLogSink
    {
        LogTable *table = nullptr;
        std::chrono::steady_clock::duration appendTotal{};
        size_t appendBatches = 0;
        size_t appendLines = 0;

        KeyIndex &Keys() override { return table->Data().Keys(); }
        void OnStarted() override {}
        void OnBatch(StreamedBatch batch) override
        {
            const size_t lines = batch.lines.size();
            const auto start = std::chrono::steady_clock::now();
            table->AppendBatch(std::move(batch));
            appendTotal += std::chrono::steady_clock::now() - start;
            ++appendBatches;
            appendLines += lines;
        }
        void OnFinished(bool /*cancelled*/) override {}
    };

    // Untimed warm-up: same shape as the [large] benchmark so the throughput
    // numbers come out comparable. Reports MB/s plus the GUI-thread AppendBatch
    // wall-time per 100 k lines streamed so M7 is visible in the test log.
    {
        LogConfigurationManager configManager;
        configManager.Load(configFile.GetFilePath());
        LogTable table(LogData{}, std::move(configManager));
        auto fileWarmup = std::make_unique<LogFile>(testFile.GetFilePath());
        table.BeginStreaming(std::move(fileWarmup));

        StreamSink sink;
        sink.table = &table;

        ParserOptions opts;
        opts.configuration = configuration;

        LogFile parseFile(testFile.GetFilePath());
        const auto start = std::chrono::steady_clock::now();
        parser.ParseStreaming(parseFile, sink, opts);
        const auto elapsed = std::chrono::steady_clock::now() - start;

        REQUIRE(table.RowCount() == logs.size());
        ReportThroughput("Stream to LogTable warm-up", elapsed, bytes, logs.size());

        const double appendMs = std::chrono::duration<double, std::milli>(sink.appendTotal).count();
        const double per100k = sink.appendLines == 0
                                   ? 0.0
                                   : appendMs * 100'000.0 / static_cast<double>(sink.appendLines);
        WARN(
            "LogTable::AppendBatch wall-time: " << appendMs << " ms over " << sink.appendBatches << " batches / "
                                                << sink.appendLines << " lines (" << per100k << " ms / 100k lines)"
        );
    }

    RunTimedSamples("Stream 1'000'000 JSON log entries to LogTable", 5, [&]() {
        LogConfigurationManager configManager;
        configManager.Load(configFile.GetFilePath());
        LogTable table(LogData{}, std::move(configManager));
        auto fileForTable = std::make_unique<LogFile>(testFile.GetFilePath());
        table.BeginStreaming(std::move(fileForTable));

        StreamSink sink;
        sink.table = &table;

        ParserOptions opts;
        opts.configuration = configuration;

        LogFile parseFile(testFile.GetFilePath());
        parser.ParseStreaming(parseFile, sink, opts);
        REQUIRE(table.RowCount() == logs.size());
    });
}

// PRD task 6.7 — cancellation-latency benchmark. Drives `ParseStreaming`
// against a 1'000'000-line fixture and asks for a stop after the first batch
// arrives. We measure the wall time between `request_stop()` and
// `OnFinished(cancelled=true)` to validate the PRD-imposed
// `ntokens × batchSizeBytes` upper bound (PRD req. 4.2.22a/b).
//
// Note: a Catch2 BENCHMARK lambda would time the full ParseStreaming call
// (Stage A startup + cancellation propagation + drain), so it cannot
// isolate the latency itself. We repeat the parse N times manually and
// summarise the latency distribution via INFO so it shows up in the test
// log. (Same rationale as the manual `RunTimedSamples` helper used above
// for `[large]` / `[wide]` / `[stream_to_table]`.)
TEST_CASE("Cancellation latency", "[.][benchmark][json_parser][cancellation]")
{
    auto logs = GenerateRandomJsonLogs(1'000'000);
    const TestJsonLogFile testFile(logs);
    const JsonParser parser;

    struct LatencySink : StreamingLogSink
    {
        KeyIndex keys;
        std::stop_source stop;
        std::chrono::steady_clock::time_point requestedAt{};
        std::chrono::steady_clock::time_point finishedAt{};
        bool cancelled = false;
        size_t batches = 0;

        KeyIndex &Keys() override { return keys; }
        void OnStarted() override {}
        void OnBatch(StreamedBatch /*batch*/) override
        {
            ++batches;
            if (batches == 1 && !stop.stop_requested())
            {
                requestedAt = std::chrono::steady_clock::now();
                stop.request_stop();
            }
        }
        void OnFinished(bool wasCancelled) override
        {
            finishedAt = std::chrono::steady_clock::now();
            cancelled = wasCancelled;
        }
    };

    constexpr int kIterations = 20;
    std::vector<double> latenciesUs;
    latenciesUs.reserve(kIterations);

    for (int i = 0; i < kIterations; ++i)
    {
        LogFile file(testFile.GetFilePath());
        LatencySink sink;
        ParserOptions opts;
        opts.stopToken = sink.stop.get_token();
        parser.ParseStreaming(file, sink, opts);
        REQUIRE(sink.cancelled);
        REQUIRE(sink.requestedAt.time_since_epoch().count() != 0);
        const auto latency = std::chrono::duration<double, std::micro>(sink.finishedAt - sink.requestedAt).count();
        latenciesUs.push_back(latency);
    }

    std::sort(latenciesUs.begin(), latenciesUs.end());
    const double median = latenciesUs[latenciesUs.size() / 2];
    const double p95 = latenciesUs[(latenciesUs.size() * 95) / 100];
    const double maxLatency = latenciesUs.back();
    // WARN is the only Catch2 macro that prints unconditionally on success, which
    // matters for benchmarks where the *number* is the deliverable. The
    // "warning" framing is Catch's; functionally this is just a report line.
    WARN(
        "Cancellation latency over " << kIterations << " runs (us): median=" << median << ", p95=" << p95
                                     << ", max=" << maxLatency
    );
    // Sanity bound: even on a slow CI box we expect cancellation to drain in
    // well under a second (PRD req. 4.2.22b). 5s is a generous safety net.
    REQUIRE(maxLatency < 5'000'000.0);
}
