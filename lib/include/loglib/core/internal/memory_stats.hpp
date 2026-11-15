/**
 * @brief Heap byte counts of the row storage, for the memory benchmark and tests.
 *
 * The counts read private state and are not part of the public API, so the classes keep them private and name
 * MemoryStats as a friend. Each function here forwards to one of them. Nothing in the library calls these functions.
 *
 * This is part of the core module.
 */
#pragma once

#include "loglib/core/enum_dictionary.hpp"
#include "loglib/core/file_line_source.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/stream_line_source.hpp"

#include <cstddef>

namespace loglib::internal
{

/** @brief Reads the memory counters that the core classes keep private. */
struct MemoryStats
{
    /** @brief Heap bytes owned by this row alone. A row whose fields live in a field arena owns none. */
    [[nodiscard]] static size_t RowOwnedBytes(const LogLine &line)
    {
        return line.OwnedMemoryBytes();
    }

    /** @brief Heap bytes held by the line offsets of a file, counting capacity. */
    [[nodiscard]] static size_t LineOffsetsBytes(const FileLineSource &source) noexcept
    {
        return source.LineOffsetsMemoryBytes();
    }

    /** @brief Heap bytes reserved by the owned-string arena of a file. */
    [[nodiscard]] static size_t OwnedStringsBytes(const FileLineSource &source) noexcept
    {
        return source.OwnedStringsMemoryBytes();
    }

    /** @brief Heap bytes of the field arenas a file holds. */
    [[nodiscard]] static size_t FieldArenasBytes(const FileLineSource &source) noexcept
    {
        return source.FieldArenasMemoryBytes();
    }

    /** @brief Heap bytes held by the line text and decoded strings of a stream, counting capacity. */
    [[nodiscard]] static size_t StreamOwnedBytes(const StreamLineSource &source) noexcept
    {
        return source.OwnedMemoryBytes();
    }

    /** @brief Approximate heap bytes used by a key index. */
    [[nodiscard]] static size_t KeyIndexBytes(const KeyIndex &keys)
    {
        return keys.EstimatedMemoryBytes();
    }

    /** @brief Approximate heap bytes used by the enum dictionaries of a table. */
    [[nodiscard]] static size_t EnumDictionariesBytes(const EnumDictionaryRegistry &registry) noexcept
    {
        return registry.EstimatedMemoryBytes();
    }
};

} // namespace loglib::internal
