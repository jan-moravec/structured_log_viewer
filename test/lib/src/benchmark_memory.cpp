// Memory-per-row benchmark. It loads generated JSON Lines rows and reports where the bytes of the loaded table sit,
// split by the type that owns them, and the total per row. The figures are deterministic, so a change in the layout of
// a row shows up as a change in a counter. The unit test in test_row_memory.cpp limits the total. See CONTRIBUTING.md
// ## Benchmarking.

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"
#include "row_memory.hpp"

#include <benchmark/benchmark.h>

#include <cstddef>
#include <memory>
#include <string>

namespace bench
{

namespace
{

struct MemoryInput
{
    GeneratedJsonTable table;
};

} // namespace

void RegisterMemoryBenchmarks()
{
    // The load happens once, outside the timed part. The timed part is the walk that adds up the memory of the rows.
    for (const Tier &tier : {FULL_TIER, SMOKE_TIER})
    {
        RegisterLarge<MemoryInput>(
            TierName("Memory per row of " + CountText(tier.rows) + " loaded JSON rows", tier),
            [rows = tier.rows, smoke = tier.smoke] {
                auto input = std::make_unique<MemoryInput>();
                input->table = LoadGeneratedJsonTable(rows, smoke ? "bench_memory_smoke.jsonl" : "bench_memory.jsonl");
                return input;
            },
            [rows = tier.rows](MemoryInput &input, benchmark::State &state, bool /*first*/) {
                RowMemory memory;
                for (const auto _ : state)
                {
                    memory = MeasureRowMemory(input.table.loaded->table);
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
}

} // namespace bench
