/**
 * @brief Measures how many bytes a loaded table holds for each row.
 *
 * The memory benchmark reports the parts and the unit test checks the total against a limit, so both go through the
 * same code. Each part is asked from the type that owns it, so the figures are deterministic and do not depend on the
 * allocator or the operating system.
 */
#pragma once

#include "common.hpp"

#include <loglib/output/file_loading.hpp>

#include <cstddef>
#include <memory>

namespace bench
{

/**
 * @brief The heap bytes of a loaded table, split by owner.
 */
struct RowMemory
{
    std::size_t rows = 0;

    /** @brief sizeof(LogLine) times the row count: the row objects themselves. */
    std::size_t lineObjects = 0;

    /** @brief Heap bytes the rows own, such as the storage of their cells. */
    std::size_t ownedByRows = 0;

    /** @brief The memory that rows of a file source take their fields from, instead of owning them. */
    std::size_t fieldArenas = 0;

    /** @brief The line offsets of the file source. */
    std::size_t lineOffsets = 0;

    /** @brief The arena of the file source, which holds the strings that are not views into the file. */
    std::size_t arena = 0;

    /** @brief The key index. */
    std::size_t keyIndex = 0;

    /** @brief The dictionaries of the enumeration columns. */
    std::size_t enumDictionaries = 0;

    [[nodiscard]] std::size_t Total() const noexcept
    {
        return lineObjects + ownedByRows + fieldArenas + lineOffsets + arena + keyIndex + enumDictionaries;
    }

    [[nodiscard]] double BytesPerRow() const noexcept
    {
        return rows == 0 ? 0.0 : static_cast<double>(Total()) / static_cast<double>(rows);
    }
};

/**
 * @brief Reads the memory of a table that was loaded from a file.
 */
[[nodiscard]] RowMemory MeasureRowMemory(const loglib::LogTable &table);

/**
 * @brief A generated JSON Lines file and the table loaded from it. The table reads from the file, so they live
 * together.
 */
struct GeneratedJsonTable
{
    std::unique_ptr<TestStructuredLogFile> file;
    std::unique_ptr<loglib::LoadResult> loaded;
};

/**
 * @brief Generates a JSON Lines file of rows records with the pinned seed and timestamps, and loads it. Throws
 * std::runtime_error when the load reports an error or a wrong row count.
 */
[[nodiscard]] GeneratedJsonTable LoadGeneratedJsonTable(std::size_t rows, const std::string &fileName);

} // namespace bench
