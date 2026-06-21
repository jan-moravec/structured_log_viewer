#pragma once

// Shared scaffolding for the parser streaming benchmarks. Callers pass a
// `ParserStreamFn` so the same timing / memory / throughput harness drives
// any parser's static `ParseStreaming` overload. See CONTRIBUTING.md
// `## Benchmarking` for PR-process docs.

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

#include <test_common/log_generator.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

/// Pinned RNG seed for the large cross-format fixture (`[json_parser][large]`,
/// `[logfmt_parser][large]`, …). Combined with `DeterministicBenchmarkTimestamps()`,
/// it makes every format consume byte-identical records so cross-format
/// lines/s is directly comparable. Always pair the two — without pinned
/// timestamps, per-record `system_clock::now()` drifts fixtures by a few
/// bytes per run.
inline constexpr std::uint32_t LARGE_FIXTURE_SEED = 0xC0FFEEu;

/// Pinned RNG seed for the wide-row fixture (`[json_parser][wide]`,
/// `[logfmt_parser][wide]`). Distinct from `LARGE_FIXTURE_SEED` to avoid
/// state collisions if both shapes load in one run. Same pairing
/// requirement with `DeterministicBenchmarkTimestamps()`.
inline constexpr std::uint32_t WIDE_FIXTURE_SEED = 0xDEC0DEu;

/// Pinned `TimestampPolicy` for benchmark fixtures: 2026-01-01 00:00:00 UTC
/// + 1 ms per record. The 1 ms step keeps the formatted-string length
/// constant across the whole fixture.
inline test_common::TimestampPolicy DeterministicBenchmarkTimestamps()
{
    // 2026-01-01T00:00:00Z = 1767225600 seconds since the Unix epoch.
    constexpr std::chrono::seconds BENCHMARK_BASE_TIME_EPOCH{1767225600};
    return {
        .baseTime = std::chrono::system_clock::time_point{BENCHMARK_BASE_TIME_EPOCH},
        .interval = std::chrono::milliseconds{1},
    };
}

/// Skip the current `TEST_CASE` under Debug. Debug disables IPO/LTO and
/// leaves assertions on, so numbers are not comparable to release.
inline void RequireReleaseBuildForBenchmarks()
{
#ifndef NDEBUG
    SKIP("Benchmarks require a release build (Debug disables IPO/LTO and "
         "leaves assertions enabled, so numbers are not comparable). "
         "Rebuild with: cmake --preset release  (or relwithdebinfo).");
#endif
}

/// Emit a single-run throughput line (uses `WARN` so it prints on success).
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

/// Run `fn` `samples` times and compute mean/low/high/stddev. Used instead
/// of `BENCHMARK_ADVANCED` because Catch2's iteration estimator + 100-sample
/// bootstrap added 3-5x wall-time on the multi-second-per-sample fixtures.
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

/// Throughput overload. Reports MB/s (mean/low/high/stddev) and lines/s on
/// one `WARN` line; mean MB/s is the regression-gate input. The MB/s
/// stddev is a first-order Taylor approximation around the mean.
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

/// Heap bytes owned by a parsed `LogTable` (mmap'd file content excluded).
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

/// Process peak working set (Windows) / max RSS (POSIX), in bytes.
/// OS-level cross-check against `StructuralBytes`; noisier across runs.
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
#ifdef __APPLE__
        // Darwin reports `ru_maxrss` in bytes. The system `rusage` layout
        // wraps the field through a glibc-style union on some platforms,
        // so the union-access lint is unavoidable here.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
        return static_cast<std::size_t>(usage.ru_maxrss);
#else
        // Linux/BSDs report `ru_maxrss` in kilobytes; see the macOS
        // branch above for the union-access NOLINT rationale.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
        return static_cast<std::size_t>(usage.ru_maxrss) * 1024U;
#endif
    }
    return 0;
#else
    return 0;
#endif
}

/// Sink mirroring `LogModel::OnBatch`: forwards every batch to
/// `LogTable::AppendBatch` (the GUI-thread back-fill loop).
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

/// `LogConfiguration` with one `Type::Time` column for `timestamp`, so the
/// parser does real inline timestamp promotion (the GUI's typical shape).
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

/// Parser-agnostic streaming entry point: matches the static
/// `ParseStreaming` overload shared by every parser, so benchmarks can plug
/// in `JsonParser::ParseStreaming`, `LogfmtParser::ParseStreaming`, etc.
using ParserStreamFn = std::function<void(
    loglib::FileLineSource &,
    loglib::LogParseSink &,
    const loglib::ParserOptions &,
    loglib::internal::AdvancedParserOptions
)>;

/// Results of one end-to-end streaming run. The timed window covers
/// configuration load + `LogTable` construction + mmap + `ParseStreaming`
/// + `~LogTable`. Sink stats and row count are copied out before the inner
/// scope closes so the caller can inspect them after the timer stops.
struct StreamingRunResult
{
    std::chrono::steady_clock::duration elapsed{};
    std::chrono::steady_clock::duration appendTotal{};
    std::size_t appendBatches = 0;
    std::size_t appendLines = 0;
    std::size_t rowCount = 0;
    /// Populated only when `captureMemory == true`. Sampled while the
    /// `LogTable` is still alive so the counters reflect steady state.
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

        // Mirror `MainWindow::OpenJsonStreaming`: the parser borrows the
        // same `FileLineSource` the table owns, so Stage C's offsets and
        // owned-string arena land in the `LogFile` value accesses read
        // from. Splitting them would dangle `LogValue`s and under-count
        // structural bytes.
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
            // Sample inside the `LogTable` scope; sampling after `~LogTable`
            // would skew the working-set delta.
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

/// Run one streaming benchmark: an untimed warm-up that reports
/// `AppendBatch` wall-time and structural memory, then `samples` timed
/// end-to-end runs through the throughput overload.
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
        // Memory introspection runs on the warm-up only: structural bytes
        // are deterministic across runs, so one sample suffices and timed
        // samples don't pay for the walk.
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
