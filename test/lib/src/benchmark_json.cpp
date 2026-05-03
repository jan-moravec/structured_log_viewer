// Parser benchmarks for `loglib`. See CONTRIBUTING.md `## Benchmarking`
// for the PR-process docs (regression gate, sample interpretation, how
// to run). Debug builds skip these cases automatically — see
// `BENCHMARK_REQUIRES_RELEASE_BUILD`.

#include "common.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_factory.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/log_table.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/stop_token.hpp>

#include <test_common/log_generator.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// psapi.h needs windows.h above it.
#include <psapi.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

using namespace loglib;
using test_common::GenerateRandomJsonLogs;
using test_common::GenerateWideJsonLogs;

namespace
{

/// Skip the current `TEST_CASE` when running under a Debug build. Debug
/// disables IPO/LTO and leaves assertions enabled, so the numbers are not
/// comparable to a release-config measurement and would mislead a
/// regression-gate reviewer. `NDEBUG` is the right gate: it is defined for
/// `Release` and `RelWithDebInfo` (both have IPO/LTO enabled in this
/// project's top-level `CMakeLists.txt`) and undefined for `Debug`.
inline void RequireReleaseBuildForBenchmarks()
{
#ifndef NDEBUG
    SKIP("Benchmarks require a release build (Debug disables IPO/LTO and "
         "leaves assertions enabled, so numbers are not comparable). "
         "Rebuild with: cmake --preset release  (or relwithdebinfo).");
#endif
}

/// Emit a single-run throughput line. `WARN` is used so it prints on success.
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

struct ThroughputInputs
{
    std::size_t bytes = 0;
    std::size_t lines = 0;
};

struct SampleStats
{
    std::chrono::nanoseconds mean;
    std::chrono::nanoseconds low;
    std::chrono::nanoseconds high;
    double stddevNs;
};

/// Manual replacement for Catch2's `BENCHMARK_ADVANCED`: just runs `fn`
/// `samples` times and computes mean / low / high / stddev of the wall-
/// clock. Avoids the Catch2 iteration-estimation pass and 100-resample
/// bootstrap, which together added 3-5x wall-time on multi-second-per-
/// sample fixtures and timed the 1M-line streaming case out at 30 min.
template <typename Fn> SampleStats CollectSamples(std::size_t samples, Fn &&fn)
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

    return SampleStats{mean, low, high, stddevNs};
}

template <typename Fn> void RunTimedSamples(const char *label, std::size_t samples, Fn &&fn)
{
    const SampleStats stats = CollectSamples(samples, std::forward<Fn>(fn));
    using ms = std::chrono::duration<double, std::milli>;
    WARN(
        label << " (samples=" << samples << "): mean=" << ms(stats.mean).count() << " ms, low=" << ms(stats.low).count()
              << " ms, high=" << ms(stats.high).count() << " ms, stddev=" << (stats.stddevNs / 1'000'000.0) << " ms"
    );
}

/// Throughput overload. Reports steady-state MB/s (mean / low / high /
/// stddev) and lines/s on the same WARN line — that mean-MB/s is the
/// canonical regression-gate input. `low`/`high` MB/s are derived from
/// the matching `high`/`low` elapsed; the MB/s stddev uses a first-order
/// Taylor approximation around the mean.
template <typename Fn>
void RunTimedSamples(const char *label, std::size_t samples, ThroughputInputs throughput, Fn &&fn)
{
    const SampleStats stats = CollectSamples(samples, std::forward<Fn>(fn));
    using ms = std::chrono::duration<double, std::milli>;
    using s = std::chrono::duration<double>;

    const double meanSec = s(stats.mean).count();
    const double lowSec = s(stats.low).count();
    const double highSec = s(stats.high).count();
    const double bytesMB = static_cast<double>(throughput.bytes) / (1024.0 * 1024.0);
    const double linesD = static_cast<double>(throughput.lines);
    const double meanMBps = meanSec == 0.0 ? 0.0 : bytesMB / meanSec;
    const double highMBps = lowSec == 0.0 ? 0.0 : bytesMB / lowSec;
    const double lowMBps = highSec == 0.0 ? 0.0 : bytesMB / highSec;
    const double meanLinesPerSec = meanSec == 0.0 ? 0.0 : linesD / meanSec;
    const double meanNs = static_cast<double>(stats.mean.count());
    const double stddevMBps = meanNs == 0.0 ? 0.0 : meanMBps * (stats.stddevNs / meanNs);

    WARN(
        label << " (samples=" << samples << "): mean=" << ms(stats.mean).count() << " ms, low=" << ms(stats.low).count()
              << " ms, high=" << ms(stats.high).count() << " ms, stddev=" << (stats.stddevNs / 1'000'000.0) << " ms | "
              << meanMBps << " MB/s mean (low=" << lowMBps << ", high=" << highMBps << ", stddev=" << stddevMBps
              << "), " << meanLinesPerSec << " lines/s mean"
    );
}

/// Breakdown of structural bytes owned by a parsed `LogTable`. Each member
/// counts heap bytes attributable to `loglib`'s internal data structures —
/// the mmap'd file content is excluded because the OS shares it with the
/// page cache.
struct StructuralBytes
{
    std::size_t lines = 0;
    std::size_t lineOffsets = 0;
    std::size_t ownedStrings = 0;
    std::size_t keyIndex = 0;

    std::size_t Total() const noexcept
    {
        return lines + lineOffsets + ownedStrings + keyIndex;
    }
};

StructuralBytes ComputeStructuralBytes(const LogTable &table)
{
    StructuralBytes result;
    const LogData &data = table.Data();

    for (const LogLine &line : data.Lines())
    {
        result.lines += line.OwnedMemoryBytes();
    }
    if (const FileLineSource *fileSource = data.FrontFileSource())
    {
        const LogFile &file = fileSource->File();
        result.lineOffsets += file.LineOffsetsMemoryBytes();
        result.ownedStrings += file.OwnedStringsMemoryBytes();
    }
    result.keyIndex = data.Keys().EstimatedMemoryBytes();
    return result;
}

/// Returns the process's peak working set (Windows) / max resident set
/// size (POSIX), in bytes. Used to give an OS-level cross-check against
/// the structural-bytes sum; varies more across runs than the structural
/// number, hence "informational".
std::size_t SamplePeakWorkingSetBytes()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (::GetProcessMemoryInfo(::GetCurrentProcess(), &counters, sizeof(counters)) != 0)
    {
        return static_cast<std::size_t>(counters.PeakWorkingSetSize);
    }
    return 0;
#elif defined(__unix__) || defined(__APPLE__)
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) == 0)
    {
#if defined(__APPLE__)
        // Darwin reports `ru_maxrss` in bytes.
        return static_cast<std::size_t>(usage.ru_maxrss);
#else
        // Linux/BSDs report `ru_maxrss` in kilobytes.
        return static_cast<std::size_t>(usage.ru_maxrss) * 1024U;
#endif
    }
    return 0;
#else
    return 0;
#endif
}

/// Sink that mirrors `LogModel::OnBatch`: every batch is forwarded to
/// `LogTable::AppendBatch`, which runs the GUI-thread back-fill loop.
struct StreamSink : LogParseSink
{
    LogTable *table = nullptr;
    std::chrono::steady_clock::duration appendTotal{};
    size_t appendBatches = 0;
    size_t appendLines = 0;

    KeyIndex &Keys() override
    {
        return table->Keys();
    }
    void OnStarted() override
    {
    }
    void OnBatch(StreamedBatch batch) override
    {
        const size_t lines = batch.lines.size();
        const auto start = std::chrono::steady_clock::now();
        table->AppendBatch(std::move(batch));
        appendTotal += std::chrono::steady_clock::now() - start;
        ++appendBatches;
        appendLines += lines;
    }
    void OnFinished(bool /*cancelled*/) override
    {
    }
};

/// Build a `LogConfiguration` with one `Type::time` column for `timestamp`,
/// matching the GUI's typical column shape so the parser has real inline
/// timestamp-promotion work to do.
std::shared_ptr<const LogConfiguration> MakeTimestampConfiguration()
{
    LogConfiguration baseConfig;
    LogConfiguration::Column timestampColumn;
    timestampColumn.header = "timestamp";
    timestampColumn.keys = {"timestamp"};
    timestampColumn.type = LogConfiguration::Type::time;
    timestampColumn.parseFormats = {"%FT%T"};
    timestampColumn.printFormat = "%F %H:%M:%S";
    baseConfig.columns.push_back(std::move(timestampColumn));
    return std::make_shared<LogConfiguration>(std::move(baseConfig));
}

/// One end-to-end streaming run inside the `start`/`elapsed` window:
/// `LogConfigurationManager::Load` + `LogTable` construction + two mmap
/// opens + `BeginStreaming` + `ParseStreaming` + `~LogTable` (frees all
/// `LogLine`s). Sink stats and row count are copied out before the inner
/// block closes so the caller can inspect them after the timer stops.
struct StreamingRunResult
{
    std::chrono::steady_clock::duration elapsed{};
    std::chrono::steady_clock::duration appendTotal{};
    size_t appendBatches = 0;
    size_t appendLines = 0;
    size_t rowCount = 0;
    /// Populated only when the caller passes `captureMemory = true`. The
    /// sample is taken with the `LogTable` still alive so the structural
    /// byte counters reflect the steady-state post-parse layout.
    bool memoryCaptured = false;
    StructuralBytes structuralBytes{};
    std::size_t peakWorkingSetBytes = 0;
    std::size_t peakWorkingSetDeltaBytes = 0;
};

StreamingRunResult RunStreamingFlow(
    const JsonParser &parser,
    const std::filesystem::path &configPath,
    const std::filesystem::path &logPath,
    std::shared_ptr<const LogConfiguration> configuration,
    bool captureMemory = false
)
{
    StreamingRunResult result;
    internal::AdvancedParserOptions advanced;

    const std::size_t peakBefore = captureMemory ? SamplePeakWorkingSetBytes() : 0;

    const auto start = std::chrono::steady_clock::now();
    {
        LogConfigurationManager configManager;
        configManager.Load(configPath.string());
        LogTable table(LogData{}, std::move(configManager));

        // Mirror `MainWindow::OpenJsonStreaming`: one `FileLineSource`
        // (wrapping a `LogFile`) owned by the table, with the same source
        // borrowed by the parser. Sharing ensures Stage C's per-line
        // offsets and the owned-string arena both land in the same
        // `LogFile` the table will look at when accessing values
        // (otherwise structural-bytes would under-count and `LogValue`
        // materialisation would dangle).
        auto sourceForTable = std::make_unique<FileLineSource>(std::make_unique<LogFile>(logPath));
        FileLineSource *parseSource = sourceForTable.get();
        table.BeginStreaming(std::move(sourceForTable));

        StreamSink sink;
        sink.table = &table;

        ParserOptions opts;
        opts.configuration = configuration;

        parser.ParseStreaming(*parseSource, sink, opts, advanced);

        result.appendTotal = sink.appendTotal;
        result.appendBatches = sink.appendBatches;
        result.appendLines = sink.appendLines;
        result.rowCount = table.RowCount();

        if (captureMemory)
        {
            // Sample inside the `LogTable` scope so the live data structures
            // are still owned. `~LogTable` below frees them and would skew
            // the working-set delta if we sampled afterwards.
            result.structuralBytes = ComputeStructuralBytes(table);
            const std::size_t peakAfter = SamplePeakWorkingSetBytes();
            result.peakWorkingSetBytes = peakAfter;
            result.peakWorkingSetDeltaBytes = peakAfter > peakBefore ? peakAfter - peakBefore : 0;
            result.memoryCaptured = true;
        }
    }
    result.elapsed = std::chrono::steady_clock::now() - start;
    return result;
}

/// Drive one streaming benchmark fixture: an untimed warm-up that emits the
/// per-batch `LogTable::AppendBatch` wall-time line plus the structural-
/// memory footprint, then `samples` timed end-to-end runs through
/// `RunTimedSamples`'s throughput overload.
void RunStreamingBenchmark(
    const char *label,
    const JsonParser &parser,
    const std::filesystem::path &configPath,
    const std::filesystem::path &logPath,
    std::shared_ptr<const LogConfiguration> configuration,
    size_t expectedRows,
    size_t bytes,
    std::size_t samples
)
{
    {
        // Capture memory on the warm-up run only: the structural-bytes
        // signal is deterministic across runs, so a single sample suffices,
        // and we avoid paying for the introspection walk on every timed
        // sample.
        StreamingRunResult warmup =
            RunStreamingFlow(parser, configPath, logPath, configuration, /*captureMemory=*/true);
        REQUIRE(warmup.rowCount == expectedRows);
        ReportThroughput((std::string(label) + " warm-up").c_str(), warmup.elapsed, bytes, expectedRows);

        const double appendMs = std::chrono::duration<double, std::milli>(warmup.appendTotal).count();
        const double per100k =
            warmup.appendLines == 0 ? 0.0 : appendMs * 100'000.0 / static_cast<double>(warmup.appendLines);
        WARN(
            "LogTable::AppendBatch wall-time: " << appendMs << " ms over " << warmup.appendBatches << " batches / "
                                                << warmup.appendLines << " lines (" << per100k << " ms / 100k lines)"
        );

        if (warmup.memoryCaptured)
        {
            constexpr double kMiB = 1024.0 * 1024.0;
            const double linesD = static_cast<double>(expectedRows == 0 ? 1 : expectedRows);
            const double bytesPerLine =
                expectedRows == 0 ? 0.0 : static_cast<double>(warmup.structuralBytes.Total()) / linesD;
            const double fileBytesMiB = static_cast<double>(bytes) / kMiB;
            const double structuralBytesMiB = static_cast<double>(warmup.structuralBytes.Total()) / kMiB;
            const double structuralRatio = bytes == 0 ? 0.0 : structuralBytesMiB / fileBytesMiB;
            WARN(
                "Structural bytes (lines+offsets+ownedStrings+keys): "
                << structuralBytesMiB << " MiB (" << bytesPerLine << " B/line, "
                << static_cast<double>(warmup.structuralBytes.lines) / kMiB << " MiB lines + "
                << static_cast<double>(warmup.structuralBytes.lineOffsets) / kMiB << " MiB offsets + "
                << static_cast<double>(warmup.structuralBytes.ownedStrings) / kMiB << " MiB ownedStrings + "
                << static_cast<double>(warmup.structuralBytes.keyIndex) / kMiB
                << " MiB keys), structural/file ratio=" << structuralRatio << " | peak working set delta "
                << static_cast<double>(warmup.peakWorkingSetDeltaBytes) / kMiB << " MiB (peak "
                << static_cast<double>(warmup.peakWorkingSetBytes) / kMiB << " MiB)"
            );
        }
    }

    RunTimedSamples(label, samples, {bytes, expectedRows}, [&]() {
        StreamingRunResult run = RunStreamingFlow(parser, configPath, logPath, configuration);
        REQUIRE(run.rowCount == expectedRows);
    });
}

} // namespace

#define BENCHMARK_REQUIRES_RELEASE_BUILD() RequireReleaseBuildForBenchmarks()

// Synchronous-Parse coverage for `BufferingSink` (the sink behind the
// `loglib::ParseFile(parser, path)` free helper, used by tests and any
// non-GUI caller that wants a one-shot `LogData`). Small fixture because
// the synchronous path is not the GUI hot path -- the `[large]` and
// `[wide]` cases below cover that.
TEST_CASE("Parse and load JSON log (sync)", "[.][benchmark][json_parser][parse_sync]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    auto logs = GenerateRandomJsonLogs(10'000);
    const TestJsonLogFile testFile(logs);
    const JsonParser parser;
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    {
        const auto start = std::chrono::steady_clock::now();
        ParseResult warmup = ParseFile(parser, testFile.GetFilePath());
        const auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(warmup.data.Lines().size() == logs.size());
        REQUIRE(warmup.errors.empty());
        ReportThroughput("Parse 10'000 (sync) warm-up", elapsed, bytes, logs.size());
    }

    RunTimedSamples("Parse 10'000 JSON log entries (sync)", 5, {bytes, logs.size()}, [&]() {
        LogTable table;
        ParseResult result = ParseFile(parser, testFile.GetFilePath());
        REQUIRE(result.data.Lines().size() == testFile.Lines().size());
        REQUIRE(result.errors.empty());
        table.Update(std::move(result.data));
    });
}

// Large-file streaming benchmark (1'000'000 lines, ~170 MB). End-to-end
// GUI flow: `LogTable::BeginStreaming` + a sink that calls
// `LogTable::AppendBatch` per `OnBatch`, with a `Type::time` column so the
// streaming parser does real inline timestamp promotion.
TEST_CASE("Stream JSON log to LogTable (1'000'000 lines)", "[.][benchmark][json_parser][large]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    auto logs = GenerateRandomJsonLogs(1'000'000);
    const TestJsonLogFile testFile(logs);
    const JsonParser parser;
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    InitializeTimezoneData();

    auto configuration = MakeTimestampConfiguration();
    TestLogConfiguration configFile;
    configFile.Write(*configuration);

    RunStreamingBenchmark(
        "Stream 1'000'000 JSON log entries to LogTable",
        parser,
        configFile.GetFilePath(),
        testFile.GetFilePath(),
        configuration,
        logs.size(),
        bytes,
        4
    );
}

// Wide-row streaming benchmark (200'000 lines, ~30 fields per line).
// Stresses the per-line field-iteration cost (`InsertSorted`,
// `ExtractFieldKey`, `ParseLine`, `IsKeyInAnyColumn`). Line count is lower
// than `[large]`'s 1'000'000 so total field operations stay comparable
// (~6x fields per line gives ~6 M total field ops at 200'000 lines).
TEST_CASE("Stream JSON log to LogTable (wide, 200'000 lines)", "[.][benchmark][json_parser][wide]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    auto logs = GenerateWideJsonLogs(200'000);
    const TestJsonLogFile testFile(logs);
    const JsonParser parser;
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    InitializeTimezoneData();

    auto configuration = MakeTimestampConfiguration();
    TestLogConfiguration configFile;
    configFile.Write(*configuration);

    RunStreamingBenchmark(
        "Stream 200'000 wide JSON log entries to LogTable",
        parser,
        configFile.GetFilePath(),
        testFile.GetFilePath(),
        configuration,
        logs.size(),
        bytes,
        4
    );
}

// `LogLine::GetValue` micro-benchmark: slow path (key string -> `KeyIndex`
// -> `KeyId`) vs. fast path (cached `KeyId`). Pre-parses once so the
// timed loop measures lookup cost only.
TEST_CASE("LogLine::GetValue micro-benchmark", "[.][benchmark][log_line][get_value_micro]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    auto logs = GenerateRandomJsonLogs(10'000);
    const TestJsonLogFile testFile(logs);
    const JsonParser parser;

    ParseResult result = ParseFile(parser, testFile.GetFilePath());
    REQUIRE(result.errors.empty());
    const LogData &data = result.data;
    const std::vector<LogLine> &lines = data.Lines();
    REQUIRE(!lines.empty());

    const std::array<std::string, 5> kKeys = {"timestamp", "level", "message", "thread_id", "component"};
    std::array<KeyId, 5> keyIds{};
    for (size_t i = 0; i < kKeys.size(); ++i)
    {
        const KeyId id = data.Keys().Find(kKeys[i]);
        REQUIRE(id != kInvalidKeyId);
        keyIds[i] = id;
    }

    // `volatile` sink — `RunTimedSamples` has no `keep_memory()` analogue,
    // so without this the optimiser folds the whole walk away.
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

// Allocation footprint and `string_view` fast-path fraction. We don't
// override global `operator new` (interacts badly with Catch2 reporting
// and TBB workers); instead we count the observable structural cost of
// a parse: one heap-allocated vector per line, one owned-string
// allocation per `LogValue` holding a `std::string`, zero for values
// holding a `std::string_view`. The fast-path fraction has to stay > 0
// or the parser regressed to an all-owned-string regime.
TEST_CASE("Allocation footprint and string_view fast-path fraction", "[.][benchmark][json_parser][allocations]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    auto logs = GenerateRandomJsonLogs(1'000);
    const TestJsonLogFile testFile(logs);
    const JsonParser parser;

    ParseResult result = ParseFile(parser, testFile.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == logs.size());

    // Phase 1 changed the per-field representation from a 48 B
    // `std::variant` to a 16 B `CompactLogValue` whose tags are the new
    // ground truth for "fast path vs slow path". `IsMmapSlice` /
    // `IsOwnedString` inspect the tag without materialising a `LogValue`
    // (which would always allocate a `std::string` for the slow path),
    // so the per-value cost of this benchmark stays representative of
    // the parse hot path.
    size_t lineCount = result.data.Lines().size();
    size_t totalValues = 0;
    size_t mmapSliceValues = 0;
    size_t ownedStringValues = 0;
    for (const LogLine &line : result.data.Lines())
    {
        for (size_t i = 0; i < result.data.Keys().Size(); ++i)
        {
            const KeyId id = static_cast<KeyId>(i);
            const bool mmap = line.IsMmapSlice(id);
            const bool owned = line.IsOwnedString(id);
            if (mmap)
            {
                ++mmapSliceValues;
                ++totalValues;
            }
            else if (owned)
            {
                ++ownedStringValues;
                ++totalValues;
            }
            else if (!std::holds_alternative<std::monostate>(line.GetValue(id)))
            {
                ++totalValues;
            }
        }
    }

    const size_t totalStringValues = mmapSliceValues + ownedStringValues;
    const double fastPathFraction =
        totalStringValues == 0 ? 0.0 : static_cast<double>(mmapSliceValues) / static_cast<double>(totalStringValues);
    // One vector heap allocation per line (the compact-pair vector), plus
    // *zero* per owned string (those are coalesced in the `LogFile`
    // arena post-stream — the per-batch arena is freed when Stage C ends).
    const size_t allocUpperBound = lineCount;

    WARN(
        "Allocation footprint over " << lineCount << " lines: " << totalValues << " values, " << mmapSliceValues
                                     << " MmapSlice (fast path), " << ownedStringValues
                                     << " OwnedString (slow path), fast-path fraction=" << (fastPathFraction * 100.0)
                                     << "%, allocation upper bound=" << allocUpperBound << " (~"
                                     << (static_cast<double>(allocUpperBound) / static_cast<double>(lineCount))
                                     << "/line)"
    );

    REQUIRE(mmapSliceValues > 0);
}

// Cancellation-latency benchmark. Asks for a stop after the first batch
// arrives and times the gap between `request_stop()` and
// `OnFinished(cancelled=true)`. Validates the
// `ntokens * batchSizeBytes` upper bound on cancellation latency.
TEST_CASE("Cancellation latency", "[.][benchmark][json_parser][cancellation]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    auto logs = GenerateRandomJsonLogs(1'000'000);
    const TestJsonLogFile testFile(logs);
    const JsonParser parser;

    struct LatencySink : LogParseSink
    {
        KeyIndex keys;
        loglib::StopSource stop;
        std::chrono::steady_clock::time_point requestedAt{};
        std::chrono::steady_clock::time_point finishedAt{};
        bool cancelled = false;
        size_t batches = 0;

        KeyIndex &Keys() override
        {
            return keys;
        }
        void OnStarted() override
        {
        }
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
        FileLineSource source(std::make_unique<LogFile>(testFile.GetFilePath()));
        LatencySink sink;
        ParserOptions opts;
        opts.stopToken = sink.stop.get_token();
        parser.ParseStreaming(source, sink, opts);
        REQUIRE(sink.cancelled);
        REQUIRE(sink.requestedAt.time_since_epoch().count() != 0);
        const auto latency = std::chrono::duration<double, std::micro>(sink.finishedAt - sink.requestedAt).count();
        latenciesUs.push_back(latency);
    }

    std::sort(latenciesUs.begin(), latenciesUs.end());
    const double median = latenciesUs[latenciesUs.size() / 2];
    const double p95 = latenciesUs[(latenciesUs.size() * 95) / 100];
    const double maxLatency = latenciesUs.back();
    WARN(
        "Cancellation latency over " << kIterations << " runs (us): median=" << median << ", p95=" << p95
                                     << ", max=" << maxLatency
    );
    REQUIRE(maxLatency < 5'000'000.0);
}
