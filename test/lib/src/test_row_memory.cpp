// Limits the memory a loaded table holds for each row. The same measurement is reported by the memory benchmark.

#include "row_memory.hpp"

#include <catch2/catch_test_macros.hpp>

namespace
{

constexpr std::size_t ROW_COUNT = 100'000;

// The most bytes per row a loaded table may hold: the 138.55 measured for these 100'000 rows, plus 2 %. A change in the
// layout of a row must change this number on purpose, so that nobody makes rows larger by accident.
//
// The line offsets are counted by capacity, and the capacity of a vector depends on the standard library, so the total
// differs a little between platforms. The value was measured with MSVC. If another platform exceeds it only because of
// the vector capacity, give that platform its own limit measured there.
constexpr double MAX_BYTES_PER_ROW = 141.32;

} // namespace

TEST_CASE("A loaded table stays within the memory limit per row", "[row_memory]")
{
    const bench::GeneratedJsonTable generated = bench::LoadGeneratedJsonTable(ROW_COUNT, "test_row_memory.jsonl");
    const bench::RowMemory memory = bench::MeasureRowMemory(generated.loaded->table);
    INFO(
        "line objects " << memory.lineObjects << ", owned by rows " << memory.ownedByRows << ", line offsets "
                        << memory.lineOffsets << ", arena " << memory.arena << ", key index " << memory.keyIndex
                        << ", enum dictionaries " << memory.enumDictionaries
    );
    INFO("bytes per row " << memory.BytesPerRow());
    REQUIRE(memory.rows == ROW_COUNT);
    CHECK(memory.BytesPerRow() <= MAX_BYTES_PER_ROW);
}
