#pragma once

#include "loglib/key_index.hpp"
#include "loglib/log_configuration.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace loglib::internal
{

/**
 * @brief Rebuilds the per-column `KeyId` cache from @p configuration.
 *
 * Stateless service. The cache lives on `LogTable`; this helper
 * only writes @p cache. Each inner vector follows `Column::keys`
 * order, including `INVALID_KEY_ID` for keys not yet interned.
 *
 * @param cache Destination; replaced entirely.
 * @param configuration Column schema to resolve.
 * @param keys Intern table used for `Find`.
 */
void RefreshColumnKeyIds(
    std::vector<std::vector<KeyId>> &cache, const LogConfiguration &configuration, const KeyIndex &keys
);

/**
 * @brief Resolves only columns affected by @p newKeys, and resizes
 * @p cache to `configuration.columns.size()` so a shrinking Load
 * cannot leave orphan entries.
 *
 * @param cache Per-column KeyId lists owned by `LogTable`.
 * @param configuration Column schema after `AppendKeys` / Load.
 * @param keys Intern table used for `Find`.
 * @param newKeys Keys that landed in this batch (or empty to only resize).
 */
void RefreshColumnKeyIdsForKeys(
    std::vector<std::vector<KeyId>> &cache,
    const LogConfiguration &configuration,
    const KeyIndex &keys,
    const std::vector<std::string> &newKeys
);

/**
 * @brief Rotates the cache entry at @p srcIndex to @p destIndex.
 * No-op when either index is out of range or they are equal.
 *
 * @param cache Per-column KeyId lists owned by `LogTable`.
 * @param srcIndex Source column index.
 * @param destIndex Destination column index.
 */
void MoveColumnKeyIds(std::vector<std::vector<KeyId>> &cache, size_t srcIndex, size_t destIndex);

} // namespace loglib::internal
