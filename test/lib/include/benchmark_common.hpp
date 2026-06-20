#pragma once

// Shared scaffolding for the parser streaming benchmarks (`benchmark_json.cpp`,
// `benchmark_logfmt.cpp`). The streaming flow is parser-agnostic: callers pass
// a `ParserStreamFn` so the same timing / memory / throughput harness drives
// any parser's static `ParseStreaming` overload. See CONTRIBUTING.md
// `## Benchmarking` for the PR-process docs.

#include <loglib/enum_dictionary.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_table.hpp>
#include <loglib/parser_options.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// psapi.h needs windows.h above it.
#include <psapi.h>
#elif defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace bench
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
inline void ReportThroughput(const char *label, std::chrono::nanoseconds elapsed, std::size_t bytes, std::size_t lines)
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
template <typename Fn> SampleStats CollectSamples(std::size_t samples, Fn fn)
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
    const auto low = *std::ranges::min_element(elapsed);
    const auto high = *std::ranges::max_element(elapsed);

    const auto meanNs = static_cast<double>(mean.count());
    double sqAccum = 0.0;
    for (const auto &e : elapsed)
    {
        const double d = static_cast<double>(e.count()) - meanNs;
        sqAccum += d * d;
    }
    const double stddevNs = std::sqrt(sqAccum / static_cast<double>(samples));

    return SampleStats{.mean = mean, .low = low, .high = high, .stddevNs = stddevNs};
}

template <typename Fn> void RunTimedSamples(const char *label, std::size_t samples, Fn &&fn)
{
    const SampleStats stats = CollectSamples(samples, std::forward<Fn>(fn));
    using Ms = std::chrono::duration<double, std::milli>;
    WARN(
        label << " (samples=" << samples << "): mean=" << Ms(stats.mean).count() << " ms, low=" << Ms(stats.low).count()
              << " ms, high=" << Ms(stats.high).count() << " ms, stddev=" << (stats.stddevNs / 1'000'000.0) << " ms"
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
    using Ms = std::chrono::duration<double, std::milli>;
    using S = std::chrono::duration<double>;

    const double meanSec = S(stats.mean).count();
    const double lowSec = S(stats.low).count();
    const double highSec = S(stats.high).count();
    const double bytesMB = static_cast<double>(throughput.bytes) / (1024.0 * 1024.0);
    const auto linesD = static_cast<double>(throughput.lines);
    const double meanMBps = meanSec == 0.0 ? 0.0 : bytesMB / meanSec;
    const double highMBps = lowSec == 0.0 ? 0.0 : bytesMB / lowSec;
    const double lowMBps = highSec == 0.0 ? 0.0 : bytesMB / highSec;
    const double meanLinesPerSec = meanSec == 0.0 ? 0.0 : linesD / meanSec;
    const auto meanNs = static_cast<double>(stats.mean.count());
    const double stddevMBps = meanNs == 0.0 ? 0.0 : meanMBps * (stats.stddevNs / meanNs);

    WARN(
        label << " (samples=" << samples << "): mean=" << Ms(stats.mean).count() << " ms, low=" << Ms(stats.low).count()
              << " ms, high=" << Ms(stats.high).count() << " ms, stddev=" << (stats.stddevNs / 1'000'000.0) << " ms | "
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

struct StructuralBytesWithEnums : StructuralBytes
{
    std::size_t enumDictionaries = 0;
};

inline StructuralBytes ComputeStructuralBytes(const loglib::LogTable &table)
{
    StructuralBytes result;
    const loglib::LogData &data = table.Data();

    for (const loglib::LogLine &line : data.Lines())
    {
        result.lines += line.OwnedMemoryBytes();
    }
    if (const loglib::FileLineSource *fileSource = data.FrontFileSource())
    {
        const loglib::LogFile &file = fileSource->File();
        result.lineOffsets += file.LineOffsetsMemoryBytes();
        result.ownedStrings += file.OwnedStringsMemoryBytes();
    }
    result.keyIndex = data.Keys().EstimatedMemoryBytes();
    return result;
}

inline StructuralBytesWithEnums ComputeStructuralBytesWithEnums(const loglib::LogTable &table)
{
    StructuralBytesWithEnums result;
    static_cast<StructuralBytes &>(result) = ComputeStructuralBytes(table);
    result.enumDictionaries = table.EnumDictionaries().EstimatedMemoryBytes();
    return result;
}

/// Returns the process's peak working set (Windows) / max resident set
/// size (POSIX), in bytes. Used to give an OS-level cross-check against
/// the structural-bytes sum; varies more across runs than the structural
/// number, hence "informational".
inline std::size_t SamplePeakWorkingSetBytes()
{
#ifdef _WIN32
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
struct StreamSink : loglib::LogParseSink
{
    loglib::LogTable *table = nullptr;
    std::chrono::steady_clock::duration appendTotal{};
    std::size_t appendBatches = 0;
    std::size_t appendLines = 0;

    loglib::KeyIndex &Keys() override
    {
        return table->Keys();
    }
    void OnStarted() override
    {
    }
    void OnBatch(loglib::StreamedBatch batch) override
    {
        const std::size_t lines = batch.lines.size();
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

/// Build a `LogConfiguration` with one `Type::Time` column for `timestamp`,
/// matching the GUI's typical column shape so the parser has real inline
/// timestamp-promotion work to do.
inline std::shared_ptr<const loglib::LogConfiguration> MakeTimestampConfiguration()
{
    loglib::LogConfiguration baseConfig;
    loglib::LogConfiguration::Column timestampColumn;
    timestampColumn.header = "timestamp";
    timestampColumn.keys = {"timestamp"};
    timestampColumn.type = loglib::LogConfiguration::Type::Time;
    timestampColumn.parseFormats = {"%FT%T"};
    timestampColumn.printFormat = "%F %H:%M:%S";
    baseConfig.columns.push_back(std::move(timestampColumn));
    return std::make_shared<loglib::LogConfiguration>(std::move(baseConfig));
}

/// Parser-agnostic streaming entry point. Matches the static
/// `ParseStreaming(FileLineSource&, LogParseSink&, const ParserOptions&,
/// internal::AdvancedParserOptions)` overload shared by every parser, so a
/// benchmark can plug in `JsonParser::ParseStreaming`,
/// `LogfmtParser::ParseStreaming`, etc.
using ParserStreamFn = std::function<void(
    loglib::FileLineSource &, loglib::LogParseSink &, const loglib::ParserOptions &,
    loglib::internal::AdvancedParserOptions
)>;

/// One end-to-end streaming run inside the `start`/`elapsed` window:
/// `LogConfigurationManager::Load` + `LogTable` construction + two mmap
/// opens + `BeginStreaming` + `ParseStreaming` + `~LogTable` (frees all
/// `LogLine`s). Sink stats and row count are copied out before the inner
/// block closes so the caller can inspect them after the timer stops.
struct StreamingRunResult
{
    std::chrono::steady_clock::duration elapsed{};
    std::chrono::steady_clock::duration appendTotal{};
    std::size_t appendBatches = 0;
    std::size_t appendLines = 0;
    std::size_t rowCount = 0;
    /// Populated only when the caller passes `captureMemory = true`. The
    /// sample is taken with the `LogTable` still alive so the structural
    /// byte counters reflect the steady-state post-parse layout.
    bool memoryCaptured = false;
    StructuralBytes structuralBytes{};
    std::size_t peakWorkingSetBytes = 0;
    std::size_t peakWorkingSetDeltaBytes = 0;
};

inline StreamingRunResult RunStreamingFlow(
    const std::filesystem::path &configPath,
    const std::filesystem::path &logPath,
    std::shared_ptr<const loglib::LogConfiguration> configuration,
    const ParserStreamFn &parserStream,
    bool captureMemory = false
)
{
    StreamingRunResult result;
    const loglib::internal::AdvancedParserOptions advanced;

    const std::size_t peakBefore = captureMemory ? SamplePeakWorkingSetBytes() : 0;

    const auto start = std::chrono::steady_clock::now();
    {
        loglib::LogConfigurationManager configManager;
        configManager.Load(configPath.string());
        loglib::LogTable table(loglib::LogData{}, std::move(configManager));

        // Mirror `MainWindow::OpenJsonStreaming`: one `FileLineSource`
        // (wrapping a `LogFile`) owned by the table, with the same source
        // borrowed by the parser. Sharing ensures Stage C's per-line
        // offsets and the owned-string arena both land in the same
        // `LogFile` the table will look at when accessing values
        // (otherwise structural-bytes would under-count and `LogValue`
        // materialisation would dangle).
        auto sourceForTable = std::make_unique<loglib::FileLineSource>(std::make_unique<loglib::LogFile>(logPath));
        loglib::FileLineSource *parseSource = sourceForTable.get();
        table.BeginStreaming(std::move(sourceForTable));

        StreamSink sink;
        sink.table = &table;

        loglib::ParserOptions opts;
        opts.configuration = std::move(configuration);

        parserStream(*parseSource, sink, opts, advanced);

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
inline void RunStreamingBenchmark(
    const char *label,
    const std::filesystem::path &configPath,
    const std::filesystem::path &logPath,
    std::shared_ptr<const loglib::LogConfiguration> configuration,
    const ParserStreamFn &parserStream,
    std::size_t expectedRows,
    std::size_t bytes,
    std::size_t samples
)
{
    {
        // Capture memory on the warm-up run only: the structural-bytes
        // signal is deterministic across runs, so a single sample suffices,
        // and we avoid paying for the introspection walk on every timed
        // sample.
        const StreamingRunResult warmup =
            RunStreamingFlow(configPath, logPath, configuration, parserStream, /*captureMemory=*/true);
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
            constexpr double MIB = 1024.0 * 1024.0;
            const auto linesD = static_cast<double>(expectedRows == 0 ? 1 : expectedRows);
            const double bytesPerLine =
                expectedRows == 0 ? 0.0 : static_cast<double>(warmup.structuralBytes.Total()) / linesD;
            const double fileBytesMiB = static_cast<double>(bytes) / MIB;
            const double structuralBytesMiB = static_cast<double>(warmup.structuralBytes.Total()) / MIB;
            const double structuralRatio = bytes == 0 ? 0.0 : structuralBytesMiB / fileBytesMiB;
            WARN(
                "Structural bytes (lines+offsets+ownedStrings+keys): "
                << structuralBytesMiB << " MiB (" << bytesPerLine << " B/line, "
                << static_cast<double>(warmup.structuralBytes.lines) / MIB << " MiB lines + "
                << static_cast<double>(warmup.structuralBytes.lineOffsets) / MIB << " MiB offsets + "
                << static_cast<double>(warmup.structuralBytes.ownedStrings) / MIB << " MiB ownedStrings + "
                << static_cast<double>(warmup.structuralBytes.keyIndex) / MIB
                << " MiB keys), structural/file ratio=" << structuralRatio << " | peak working set delta "
                << static_cast<double>(warmup.peakWorkingSetDeltaBytes) / MIB << " MiB (peak "
                << static_cast<double>(warmup.peakWorkingSetBytes) / MIB << " MiB)"
            );
        }
    }

    RunTimedSamples(label, samples, {.bytes = bytes, .lines = expectedRows}, [&]() {
        const StreamingRunResult run = RunStreamingFlow(configPath, logPath, configuration, parserStream);
        REQUIRE(run.rowCount == expectedRows);
    });
}

} // namespace bench

#define BENCHMARK_REQUIRES_RELEASE_BUILD() ::bench::RequireReleaseBuildForBenchmarks()
