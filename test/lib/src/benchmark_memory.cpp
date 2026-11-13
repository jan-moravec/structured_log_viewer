// Memory-per-row benchmark. It loads generated rows and reports where the bytes of the loaded table sit, split by the
// type that owns them, and the total per row. The figures are deterministic, so a change in the layout of a row shows
// up as a change in a counter. The unit test in test_row_memory.cpp limits the total for JSON rows. See CONTRIBUTING.md
// ## Benchmarking.
//
// The same walk runs on JSON, logfmt and Apache combined rows loaded from files, and on JSON rows that came through a
// live stream. A second benchmark times the destruction of a loaded table.

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"
#include "row_memory.hpp"

#include <loglib/core/stream_line_source.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/table/log_table.hpp>

#include <test_common/log_format.hpp>

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>

namespace bench
{

namespace
{

/**
 * A loaded table of any kind, with the objects it reads from. The file and the table live together, because the rows
 * point into them.
 */
struct MemoryInput
{
    std::unique_ptr<TestStructuredLogFile> file;
    std::unique_ptr<loglib::LoadResult> loaded;
    std::unique_ptr<loglib::LogTable> streamed;

    [[nodiscard]] const loglib::LogTable &Table() const
    {
        return streamed ? *streamed : loaded->table;
    }
};

std::unique_ptr<TestStructuredLogFile> MakeFile(
    std::size_t rows, const test_common::LogFormat &format, const std::string &name
)
{
    return std::make_unique<TestStructuredLogFile>(
        StreamedRecords{
            .count = rows,
            .seed = LARGE_FIXTURE_SEED,
            .timestamps = DeterministicBenchmarkTimestamps(),
        },
        format,
        test_common::RecordSchema{},
        name
    );
}

std::unique_ptr<MemoryInput> LoadFileInput(std::unique_ptr<TestStructuredLogFile> file, std::size_t rows)
{
    auto input = std::make_unique<MemoryInput>();
    input->file = std::move(file);
    InitializeTimezoneData();
    input->loaded = std::make_unique<loglib::LoadResult>(loglib::LoadFile(input->file->GetFilePath()));
    if (!input->loaded->errors.empty() || input->loaded->table.RowCount() != rows)
    {
        throw std::runtime_error("the generated file did not load");
    }
    return input;
}

// The JSON bytes of a file, fed to a stream source through the parser the way a live tail does.
std::unique_ptr<MemoryInput> LoadStreamInput(std::size_t rows, const std::string &name)
{
    auto input = std::make_unique<MemoryInput>();
    input->file = MakeFile(rows, test_common::JsonLines(), name);
    std::ifstream in(input->file->GetFilePath(), std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    input->streamed = std::make_unique<loglib::LogTable>();
    auto source = std::make_unique<loglib::StreamLineSource>(
        std::filesystem::path("memory_stream"), std::make_unique<InMemoryBytesProducer>(std::move(bytes))
    );
    loglib::StreamLineSource *parseSource = source.get();
    input->streamed->BeginStreaming(std::move(source));
    StreamSink sink;
    sink.table = input->streamed.get();
    loglib::JsonParser{}.ParseStreaming(*parseSource, sink);
    if (input->streamed->RowCount() != rows)
    {
        throw std::runtime_error("the generated stream did not load");
    }
    return input;
}

// The load happens once, outside the timed part. The timed part is the walk that adds up the memory of the rows.
void RegisterMemoryOf(
    const std::string &name, std::function<std::unique_ptr<MemoryInput>()> make, std::size_t rows, const Tier &tier
)
{
    RegisterLarge<MemoryInput>(
        TierName(name, tier),
        std::move(make),
        [rows](MemoryInput &input, benchmark::State &state, bool /*first*/) {
            RowMemory memory;
            for (const auto _ : state)
            {
                memory = MeasureRowMemory(input.Table());
                benchmark::DoNotOptimize(memory);
            }
            if (memory.rows != rows || memory.Total() == 0)
            {
                Fail(state, "the table has the wrong number of rows");
                return;
            }
            state.counters["line_objects_bytes"] = static_cast<double>(memory.lineObjects);
            state.counters["owned_by_rows_bytes"] = static_cast<double>(memory.ownedByRows);
            state.counters["line_offsets_bytes"] = static_cast<double>(memory.lineOffsets);
            state.counters["arena_bytes"] = static_cast<double>(memory.arena);
            state.counters["key_index_bytes"] = static_cast<double>(memory.keyIndex);
            state.counters["enum_dictionaries_bytes"] = static_cast<double>(memory.enumDictionaries);
            state.counters["total_bytes"] = static_cast<double>(memory.Total());
            state.counters["bytes_per_row"] = memory.BytesPerRow();
        },
        LargeOptions{.repetitions = tier.largeRepetitions, .iterations = FAST_LARGE_ITERATIONS}
    );
}

struct TeardownInput
{
    std::unique_ptr<TestStructuredLogFile> file;
};

} // namespace

void RegisterMemoryBenchmarks()
{
    for (const Tier &tier : {FULL_TIER, SMOKE_TIER})
    {
        const std::size_t rows = tier.rows;
        const std::string suffix = tier.smoke ? "_smoke" : "";
        RegisterMemoryOf(
            "Memory per row of " + CountText(rows) + " loaded JSON rows",
            [rows, suffix] {
                return LoadFileInput(
                    MakeFile(rows, test_common::JsonLines(), "bench_memory" + suffix + ".jsonl"), rows
                );
            },
            rows,
            tier
        );
        RegisterMemoryOf(
            "Memory per row of " + CountText(rows) + " loaded logfmt rows",
            [rows, suffix] {
                return LoadFileInput(
                    MakeFile(rows, test_common::Logfmt(), "bench_memory_logfmt" + suffix + ".logfmt"), rows
                );
            },
            rows,
            tier
        );
        RegisterMemoryOf(
            "Memory per row of " + CountText(rows) + " loaded Apache combined rows",
            [rows, suffix] {
                return LoadFileInput(
                    MakeFile(rows, test_common::ApacheCombinedFormat(), "bench_memory_apache" + suffix + ".log"), rows
                );
            },
            rows,
            tier
        );
        RegisterMemoryOf(
            "Memory per row of " + CountText(rows) + " streamed JSON rows",
            [rows, suffix] { return LoadStreamInput(rows, "bench_memory_stream" + suffix + ".jsonl"); },
            rows,
            tier
        );
    }

    // The time to destroy a loaded table, which frees every row. Loading is not timed.
    RegisterLarge<TeardownInput>(
        "Table teardown of 1'000'000 loaded JSON rows",
        [] {
            auto input = std::make_unique<TeardownInput>();
            input->file = MakeFile(FULL_TIER.rows, test_common::JsonLines(), "bench_teardown.jsonl");
            InitializeTimezoneData();
            return input;
        },
        [](TeardownInput &input, benchmark::State &state, bool /*first*/) {
            for (const auto _ : state)
            {
                auto loaded = std::make_unique<loglib::LoadResult>(loglib::LoadFile(input.file->GetFilePath()));
                if (loaded->table.RowCount() != FULL_TIER.rows)
                {
                    Fail(state, "the table has the wrong number of rows");
                    return;
                }
                const auto start = std::chrono::steady_clock::now();
                loaded.reset();
                state.SetIterationTime(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
            }
        },
        LargeOptions{.iterations = FAST_LARGE_ITERATIONS, .manualTime = true}
    );
}

} // namespace bench
