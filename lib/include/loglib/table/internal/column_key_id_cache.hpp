/**
 * @brief Keeps the key ids of each configured column up to date.
 *
 * A column may show several log keys. LogTable keeps their ids in a cache, and these functions rebuild it, extend it
 * for new keys or reorder it when a column moves.
 *
 * This is part of the table module.
 */
#pragma once

#include "loglib/core/key_index.hpp"
#include "loglib/schema/log_configuration.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace loglib::internal
{

/**
 * @brief Rebuilds the list of key ids of every column.
 *
 * The cache belongs to LogTable, and this function only fills it. Each inner list follows the order of Column::keys,
 * with INVALID_KEY_ID for a key that is not interned yet. The old content of cache is replaced.
 */
void RefreshColumnKeyIds(
    std::vector<std::vector<KeyId>> &cache, const LogConfiguration &configuration, const KeyIndex &keys
);

/**
 * @brief Looks up the key ids again only for columns that use one of the new keys.
 *
 * The cache is also resized to the number of columns, so a load that leaves fewer columns cannot leave extra entries.
 * With no new keys it only resizes.
 */
void RefreshColumnKeyIdsForKeys(
    std::vector<std::vector<KeyId>> &cache,
    const LogConfiguration &configuration,
    const KeyIndex &keys,
    const std::vector<std::string> &newKeys
);

/**
 * @brief Moves the entry of one column to another position, like moving the column. Does nothing if either index is
 * out of range or they are equal.
 */
void MoveColumnKeyIds(std::vector<std::vector<KeyId>> &cache, size_t srcIndex, size_t destIndex);

} // namespace loglib::internal
