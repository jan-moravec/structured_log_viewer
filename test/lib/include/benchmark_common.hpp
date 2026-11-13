/**
 * @brief Shared helpers of the library benchmarks: how a benchmark is registered and timed, and the fixtures they
 * share.
 *
 * Every library benchmark runs on Google Benchmark through the helpers here, so all of them follow the same rules.
 * RegisterLarge is for an operation of about 50 ms or more: a fixed number of iterations per repetition, an untimed
 * warm-up and an input built once outside the timed part. RegisterSmall is for a shorter operation, which Google
 * Benchmark repeats until a minimum time has passed. Fail marks a wrong result, which makes the benchmark executable
 * exit non-zero.
 *
 * RegisterStreaming builds on RegisterLarge for the end-to-end parse benchmarks: it parses a generated file into a
 * LogTable with any parser and reports throughput and memory counters. The fixed seeds and timestamps make every run
 * parse the same bytes. CONTRIBUTING.md, section Benchmarking, says how to run and compare the benchmarks.
 */
#pragma once

#include "common.hpp"
#include "fixture_params.hpp"
#include "row_memory.hpp"
#include "table_fixture.hpp"
#include <loglib/core/enum_dictionary.hpp>
#include <loglib/core/file_line_source.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/output/file_loading.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/schema/log_configuration.hpp>

#include <loglib/table/log_table.hpp>

#include <test_common/log_generator.hpp>

#include <benchmark/benchmark.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
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

/**
 * Repetitions of a large benchmark. Each repetition is one timed iteration.
 */
inline constexpr int LARGE_REPETITIONS = 5;

/**
 * Repetitions of a small benchmark. Each repetition runs for at least SMALL_MIN_TIME_SECONDS.
 */
inline constexpr int SMALL_REPETITIONS = 5;

/**
 * Minimum time of one repetition of a small benchmark.
 */
inline constexpr double SMALL_MIN_TIME_SECONDS = 0.3;

/**
 * Timed iterations per repetition of a large benchmark whose operation takes about 150 ms or less, and of one that
 * takes up to about 500 ms. More work per repetition averages out the noise of a shared machine. A benchmark of
 * about a second or more runs one iteration.
 */
inline constexpr int FAST_LARGE_ITERATIONS = 5;
inline constexpr int MEDIUM_LARGE_ITERATIONS = 3;

/**
 * How big a benchmark is and how often it repeats. The full tier is the one whose figures are compared between
 * builds. The smoke tier registers the same benchmarks again at a smaller size and with fewer repetitions, to check
 * that they work and to give rough figures. Its names end in " (smoke)" so the two never mix in a comparison.
 */
struct Tier
{
    std::size_t rows = 0;
    bool smoke = false;
    int largeRepetitions = LARGE_REPETITIONS;
    int smallRepetitions = SMALL_REPETITIONS;
    double smallMinTimeSeconds = SMALL_MIN_TIME_SECONDS;
};

inline constexpr Tier FULL_TIER{.rows = 1'000'000};
inline constexpr Tier SMOKE_TIER{
    .rows = 50'000,
    .smoke = true,
    .largeRepetitions = 3,
    .smallRepetitions = 3,
    .smallMinTimeSeconds = 0.1,
};

/**
 * A count with a quote between groups of three digits, as the benchmark names write it: 1'000'000.
 */
inline std::string CountText(std::size_t count)
{
    std::string digits = std::to_string(count);
    std::string text;
    for (std::size_t i = 0; i < digits.size(); ++i)
    {
        if (i != 0 && (digits.size() - i) % 3 == 0)
        {
            text += '\'';
        }
        text += digits[i];
    }
    return text;
}

/**
 * The name a benchmark gets in a tier: unchanged in the full tier, with " (smoke)" added in the smoke tier.
 */
inline std::string TierName(const std::string &name, const Tier &tier)
{
    return tier.smoke ? name + " (smoke)" : name;
}

inline std::atomic<bool> &FailureFlag()
{
    static std::atomic<bool> failed{false};
    return failed;
}

/**
 * Marks the benchmark as failed. Google Benchmark drops a skipped run from its output, so the flag is what makes
 * the executable exit non-zero.
 */
inline void Fail(benchmark::State &state, const std::string &message)
{
    FailureFlag().store(true);
    state.SkipWithError(message);
}

/**
 * True when any benchmark reported a wrong result.
 */
inline bool AnyFailure()
{
    return FailureFlag().load();
}

/**
 * How a large benchmark is timed. The default is one iteration per repetition, timed by the clock, in milliseconds.
 * Operations that take less than about a second set iterations to FAST_LARGE_ITERATIONS or MEDIUM_LARGE_ITERATIONS.
 * A benchmark that measures an interval itself sets manualTime and calls SetIterationTime in every iteration.
 */
struct LargeOptions
{
    int repetitions = LARGE_REPETITIONS;
    int iterations = 1;
    bool manualTime = false;
    benchmark::TimeUnit unit = benchmark::kMillisecond;
};

/**
 * Registers a benchmark for an operation of about 50 ms or more: five repetitions of a few timed iterations,
 * aggregates only. The first call of body gets true as its last argument; it is the warm-up, so it runs the
 * operation once untimed before the timed loop. setup builds the input on the first call, outside the timed part.
 * The input is freed after the last repetition so that large fixtures do not pile up.
 */
template <typename Input>
void RegisterLarge(
    const std::string &name,
    std::function<std::unique_ptr<Input>()> setup,
    std::function<void(Input &, benchmark::State &, bool)> body,
    LargeOptions options = {}
)
{
    struct Slot
    {
        std::function<std::unique_ptr<Input>()> setup;
        std::unique_ptr<Input> input;
        int calls = 0;
    };
    const auto slot = std::make_shared<Slot>();
    slot->setup = std::move(setup);
    benchmark::Benchmark *registered =
        benchmark::RegisterBenchmark(name, [slot, body = std::move(body), options](benchmark::State &state) {
            if (!slot->input)
            {
                slot->input = slot->setup();
            }
            const bool first = slot->calls == 0;
            body(*slot->input, state, first);
            if (++slot->calls >= options.repetitions)
            {
                slot->input.reset();
            }
        });
    registered->Unit(options.unit)
        ->Iterations(options.iterations)
        ->Repetitions(options.repetitions)
        ->ReportAggregatesOnly(true);
    if (options.manualTime)
    {
        registered->UseManualTime();
    }
    else
    {
        registered->UseRealTime();
    }
}

/**
 * Builds an input on first use and keeps it for the rest of the process. Small benchmarks share one with it.
 */
template <typename Input> class LazyInput
{
public:
    explicit LazyInput(std::function<std::unique_ptr<Input>()> make)
        : mMake(std::move(make))
    {
    }

    Input &Get()
    {
        if (!mValue)
        {
            mValue = mMake();
        }
        return *mValue;
    }

private:
    std::function<std::unique_ptr<Input>()> mMake;
    std::unique_ptr<Input> mValue;
};

template <typename Input> std::shared_ptr<LazyInput<Input>> MakeLazy(std::function<std::unique_ptr<Input>()> make)
{
    return std::make_shared<LazyInput<Input>>(std::move(make));
}

/**
 * Registers a benchmark for an operation under about 50 ms. Google Benchmark picks the iteration count so that one
 * repetition lasts at least SMALL_MIN_TIME_SECONDS. fn builds nothing itself: capture the prepared input.
 */
inline void RegisterSmall(
    const std::string &name,
    std::function<void(benchmark::State &)> fn,
    benchmark::TimeUnit unit = benchmark::kMillisecond,
    const Tier &tier = FULL_TIER
)
{
    benchmark::RegisterBenchmark(name, std::move(fn))
        ->Unit(unit)
        ->MinTime(tier.smallMinTimeSeconds)
        ->Repetitions(tier.smallRepetitions)
        ->ReportAggregatesOnly(true)
        ->UseRealTime();
}

/**
 * Heap bytes owned by a parsed LogTable (mmap'd file content excluded).
 */
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
    const RowMemory memory = MeasureRowMemory(table);
    return StructuralBytes{
        .lines = memory.ownedByRows,
        .lineOffsets = memory.lineOffsets,
        .ownedStrings = memory.arena,
        .keyIndex = memory.keyIndex,
    };
}

inline StructuralBytesWithEnums ComputeStructuralBytesWithEnums(const loglib::LogTable &table)
{
    StructuralBytesWithEnums result;
    static_cast<StructuralBytes &>(result) = ComputeStructuralBytes(table);
    result.enumDictionaries = table.EnumDictionaries().EstimatedMemoryBytes();
    return result;
}

/**
 * Process peak working set (Windows) / max RSS (POSIX), in bytes.
 * OS-level cross-check against StructuralBytes; noisier across runs.
 */
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
        // Darwin reports ru_maxrss in bytes. The system rusage layout
        // wraps the field through a glibc-style union on some platforms,
        // so the union-access lint is unavoidable here.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-union-access)
        return static_cast<std::size_t>(usage.ru_maxrss);
#else
        // Linux/BSDs report ru_maxrss in kilobytes; see the macOS
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

/**
 * Sink mirroring LogModel::OnBatch: forwards every batch to
 * LogTable::AppendBatch (the GUI-thread back-fill loop).
 */
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
    void OnBatch(loglib::RowBatch batch) override
    {
        const std::size_t lines = batch.lines.size();
        const auto start = std::chrono::steady_clock::now();
        AppendFixtureBatch(*table, std::move(batch));
        appendTotal += std::chrono::steady_clock::now() - start;
        ++appendBatches;
        appendLines += lines;
    }
    void OnFinished(bool /*cancelled*/) override
    {
    }
};

/**
 * LogConfiguration with one Type::Time column for timestamp, so the
 * parser does real inline timestamp promotion (the GUI's typical shape).
 */
inline std::shared_ptr<const loglib::LogConfiguration> MakeTimestampConfiguration()
{
    loglib::LogConfiguration baseConfig;
    loglib::Column timestampColumn;
    timestampColumn.header = "timestamp";
    timestampColumn.keys = {"timestamp"};
    timestampColumn.type = loglib::ColumnType::Time;
    timestampColumn.parseFormats = {"%FT%T"};
    timestampColumn.printFormat = "%F %H:%M:%S";
    baseConfig.columns.push_back(std::move(timestampColumn));
    return std::make_shared<loglib::LogConfiguration>(std::move(baseConfig));
}

/**
 * Parser-agnostic streaming entry point: matches the static
 * ParseStreaming overload shared by every parser, so benchmarks can plug
 * in JsonParser::ParseStreaming, LogfmtParser::ParseStreaming, etc.
 */
using ParserStreamFn =
    std::function<void(loglib::FileLineSource &, loglib::LogParseSink &, const loglib::ParserOptions &)>;

/**
 * Results of one end-to-end streaming run. The timed window covers
 * configuration load + LogTable construction + mmap + ParseStreaming
 * + ~LogTable. Sink stats and row count are copied out before the inner
 * scope closes so the caller can inspect them after the timer stops.
 */
struct StreamingRunResult
{
    std::chrono::steady_clock::duration elapsed{};
    std::chrono::steady_clock::duration appendTotal{};
    std::size_t appendBatches = 0;
    std::size_t appendLines = 0;
    std::size_t rowCount = 0;
    /**
     * Populated only when captureMemory == true. Sampled while the
     * LogTable is still alive so the counters reflect steady state.
     */
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

    const std::size_t peakBefore = captureMemory ? SamplePeakWorkingSetBytes() : 0;

    const auto start = std::chrono::steady_clock::now();
    {
        loglib::LogConfigurationManager configManager;
        configManager.Load(configPath.string());
        loglib::LogTable table(configManager.Configuration());
        // The benchmarks measure parsing in the machine's zone, as the application does.
        table.SetDisplayTimeZone(loglib::ProcessDefaultTimeZone());
        table.SetDefaultSourceTimeZone(loglib::ProcessDefaultTimeZone());

        // Mirror MainWindow::OpenJsonStreaming: the parser borrows the
        // same FileLineSource the table owns, so Stage C's offsets and
        // owned-string arena land in the FileLineSource value accesses read
        // from. Splitting them would dangle LogValues and under-count
        // structural bytes.
        auto sourceForTable = std::make_unique<loglib::FileLineSource>(logPath);
        loglib::FileLineSource *parseSource = sourceForTable.get();
        table.BeginStreaming(std::move(sourceForTable));

        StreamSink sink;
        sink.table = &table;

        loglib::ParserOptions opts;
        opts.timeColumns = loglib::TimeColumnsOf(*configuration);
        opts.defaultSourceTimeZone = table.DefaultSourceTimeZone();

        parserStream(*parseSource, sink, opts);

        result.appendTotal = sink.appendTotal;
        result.appendBatches = sink.appendBatches;
        result.appendLines = sink.appendLines;
        result.rowCount = table.RowCount();

        if (captureMemory)
        {
            // Sample inside the LogTable scope; sampling after ~LogTable
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

/**
 * Loads a file without a configuration and returns the configuration that detection made for it. A benchmark that
 * uses it as the declared configuration parses the same file with every column and type named up front.
 */
inline std::shared_ptr<const loglib::LogConfiguration> DetectedConfiguration(const std::string &path)
{
    const loglib::LoadResult loaded = loglib::LoadFile(path);
    return std::make_shared<loglib::LogConfiguration>(loaded.table.Configuration().Configuration());
}

/**
 * The input of one streaming benchmark: the log file to parse and the parser that reads it.
 */
struct StreamingFixture
{
    std::unique_ptr<TestStructuredLogFile> logFile;
    ParserStreamFn parser;
    /**
     * Anything that must stay alive while the benchmark runs, such as a registered extra template.
     */
    std::shared_ptr<void> keepAlive;
};

struct StreamingInput
{
    StreamingFixture fixture;
    TestLogConfiguration configFile;
    std::shared_ptr<const loglib::LogConfiguration> configuration;
    std::size_t bytes = 0;
    std::size_t rows = 0;
    StreamingRunResult probe;
};

/**
 * Registers an end-to-end streaming benchmark. The first call parses once untimed to capture the memory counters
 * (AppendBatch wall time, structural bytes, peak working set); the timed repetitions follow. Each repetition checks
 * the row count after the timed part. With parseLoopOnly the time of an iteration excludes the LogTable::AppendBatch
 * time of the sink. With declaredConfiguration the table starts with the configuration that detection makes for the
 * file, so every column and type is declared; otherwise only the timestamp column is.
 */
inline void RegisterStreaming(
    const std::string &name,
    std::function<StreamingFixture()> makeFixture,
    bool parseLoopOnly = false,
    int iterations = MEDIUM_LARGE_ITERATIONS,
    int repetitions = LARGE_REPETITIONS,
    bool declaredConfiguration = false
)
{
    RegisterLarge<StreamingInput>(
        name,
        [makeFixture = std::move(makeFixture), declaredConfiguration] {
            auto input = std::make_unique<StreamingInput>();
            input->fixture = makeFixture();
            input->bytes = std::filesystem::file_size(input->fixture.logFile->GetFilePath());
            input->rows = input->fixture.logFile->RecordCount();
            InitializeTimezoneData();
            input->configuration = declaredConfiguration ? DetectedConfiguration(input->fixture.logFile->GetFilePath())
                                                         : MakeTimestampConfiguration();
            input->configFile.Write(*input->configuration);
            return input;
        },
        [parseLoopOnly](StreamingInput &input, benchmark::State &state, bool first) {
            const std::filesystem::path configPath = input.configFile.GetFilePath();
            const std::filesystem::path logPath = input.fixture.logFile->GetFilePath();
            if (first)
            {
                input.probe = RunStreamingFlow(
                    configPath, logPath, input.configuration, input.fixture.parser, /*captureMemory=*/true
                );
            }
            std::size_t rows = input.probe.rowCount;
            for (const auto _ : state)
            {
                const StreamingRunResult run =
                    RunStreamingFlow(configPath, logPath, input.configuration, input.fixture.parser);
                rows = run.rowCount;
                if (parseLoopOnly)
                {
                    state.SetIterationTime(std::chrono::duration<double>(run.elapsed - run.appendTotal).count());
                }
            }
            if (rows != input.rows)
            {
                Fail(state, "parsed " + std::to_string(rows) + " rows, expected " + std::to_string(input.rows));
                return;
            }
            const auto iterations = static_cast<std::int64_t>(state.iterations());
            state.SetBytesProcessed(iterations * static_cast<std::int64_t>(input.bytes));
            state.SetItemsProcessed(iterations * static_cast<std::int64_t>(input.rows));

            constexpr double MIB = 1024.0 * 1024.0;
            const StreamingRunResult &probe = input.probe;
            const double appendMs = std::chrono::duration<double, std::milli>(probe.appendTotal).count();
            state.counters["AppendBatch_ms"] = appendMs;
            state.counters["structural_MiB"] = static_cast<double>(probe.structuralBytes.Total()) / MIB;
            state.counters["structural_B_per_line"] =
                input.rows == 0 ? 0.0
                                : static_cast<double>(probe.structuralBytes.Total()) / static_cast<double>(input.rows);
            state.counters["structural_to_file_ratio"] =
                input.bytes == 0
                    ? 0.0
                    : static_cast<double>(probe.structuralBytes.Total()) / static_cast<double>(input.bytes);
            state.counters["peak_ws_delta_MiB"] = static_cast<double>(probe.peakWorkingSetDeltaBytes) / MIB;
            state.counters["peak_ws_MiB"] = static_cast<double>(probe.peakWorkingSetBytes) / MIB;
        },
        LargeOptions{.repetitions = repetitions, .iterations = iterations, .manualTime = parseLoopOnly}
    );
}

/**
 * Registers a benchmark of LoadFile on a generated file, the path the application and slp use. The table starts with no
 * configuration, or with the configuration that detection makes for the file when declaredConfiguration is set. The
 * check is the row count; the load must report no errors.
 */
inline void RegisterLoadFile(
    const std::string &name,
    std::function<std::unique_ptr<TestStructuredLogFile>()> makeFile,
    bool declaredConfiguration = false,
    int iterations = FAST_LARGE_ITERATIONS
)
{
    struct LoadInput
    {
        std::unique_ptr<TestStructuredLogFile> file;
        std::optional<loglib::LogConfiguration> configuration;
        std::size_t bytes = 0;
    };
    RegisterLarge<LoadInput>(
        name,
        [makeFile = std::move(makeFile), declaredConfiguration] {
            auto input = std::make_unique<LoadInput>();
            input->file = makeFile();
            input->bytes = std::filesystem::file_size(input->file->GetFilePath());
            InitializeTimezoneData();
            if (declaredConfiguration)
            {
                input->configuration = *DetectedConfiguration(input->file->GetFilePath());
            }
            return input;
        },
        [](LoadInput &input, benchmark::State &state, bool) {
            const std::filesystem::path path = input.file->GetFilePath();
            const auto load = [&] {
                loglib::LoadOptions options;
                options.configuration = input.configuration;
                return loglib::LoadFile(path, std::move(options));
            };
            std::size_t rows = 0;
            std::size_t errors = 0;
            for (const auto _ : state)
            {
                const loglib::LoadResult loaded = load();
                rows = loaded.table.RowCount();
                errors = loaded.errors.size();
            }
            if (rows != input.file->RecordCount() || errors != 0)
            {
                Fail(state, "loaded " + std::to_string(rows) + " rows and " + std::to_string(errors) + " errors");
                return;
            }
            const auto iterationCount = static_cast<std::int64_t>(state.iterations());
            state.SetBytesProcessed(iterationCount * static_cast<std::int64_t>(input.bytes));
            state.SetItemsProcessed(iterationCount * static_cast<std::int64_t>(rows));
        },
        LargeOptions{.iterations = iterations}
    );
}

} // namespace bench
