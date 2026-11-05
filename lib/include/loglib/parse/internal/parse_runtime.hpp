/**
 * @brief Per-thread scratch state shared by the parsing pipelines.
 *
 * Each worker keeps a cache from key text to key id and the last parsed timestamp per time column, so repeated work is
 * skipped. The static pipeline and the live-stream loop share this state. This header defines it and builds the time
 * column specs from a configuration.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/key_index.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/transparent_string_hash.hpp"
#include "loglib/schema/column_schema.hpp"
#include "loglib/time/internal/timestamp_promotion.hpp"

#include <tsl/robin_map.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace loglib::internal
{

/**
 * @brief Builds a TimeColumnSpec for each column of type Time, in order. Columns of another type are skipped. Every
 * spec reads naive timestamps in timeZone and uses referenceTime for formats without a year.
 */
std::vector<TimeColumnSpec> BuildTimeColumnSpecs(
    KeyIndex &keys,
    std::span<const Column> columns,
    const TimeZoneContext &timeZone,
    std::chrono::system_clock::time_point referenceTime
);

/**
 * @brief The key ids of all time columns in specs, sorted and without duplicates. It is what
 * RowBatch::convertedTimeKeys holds.
 */
std::vector<KeyId> ConvertedTimeKeys(std::span<const TimeColumnSpec> specs);

/**
 * @brief Each worker's own map from key text to key id. A hit needs no allocation, and a miss asks the shared index
 * once and stores the answer.
 */
struct PerWorkerKeyCache
{
    tsl::robin_map<std::string, KeyId, TransparentStringHash, TransparentStringEqual> map;
};

/**
 * @brief Scratch data that each worker keeps, used by both the static pipeline and the streaming loop.
 *
 * It holds the key cache and, for each time column, what the last successful parse was, so time columns can be
 * converted while parsing.
 */
struct WorkerScratchBase
{
    PerWorkerKeyCache keyCache;
    std::vector<std::optional<LastValidTimestampParse>> lastValidTimestamps;
    TimestampParseScratch tsScratch;
    std::vector<LastTimestampBytesHit> lastBytesHits;

    void EnsureTimeColumnCapacity(size_t n)
    {
        if (lastValidTimestamps.size() < n)
        {
            lastValidTimestamps.resize(n);
        }
        if (lastBytesHits.size() < n)
        {
            lastBytesHits.resize(n);
        }
    }

    /**
     * @brief Converts the time columns of a line to timestamps, in place.
     *
     * ownedArena holds the bytes behind the line's OwnedString values. The static pipeline passes its staging buffer
     * for the batch, and the streaming loop passes an empty one, because the line's source resolves them.
     */
    void PromoteTimestamps(LogLine &line, std::span<const TimeColumnSpec> timeColumns, std::string_view ownedArena)
    {
        if (timeColumns.empty())
        {
            return;
        }
        PromoteLineTimestamps(line, timeColumns, lastValidTimestamps, lastBytesHits, tsScratch, ownedArena);
    }
};

/**
 * @brief The shared scratch plus scratch for one format, such as a simdjson parser and its padded buffer. The static
 * pipeline uses it.
 */
template <class UserState> struct WorkerScratch : WorkerScratchBase
{
    UserState user;
};

/**
 * @brief Returns the id of a key through the worker's cache, or straight from the index if cache is null. On a miss
 * the key's bytes are copied into the cache.
 */
inline KeyId InternKeyVia(std::string_view key, KeyIndex &keys, PerWorkerKeyCache *cache)
{
    if (cache == nullptr)
    {
        return keys.GetOrInsert(key);
    }
    if (const auto it = cache->map.find(key); it != cache->map.end())
    {
        return it->second;
    }
    const KeyId id = keys.GetOrInsert(key);
    cache->map.emplace(std::string(key), id);
    return id;
}

} // namespace loglib::internal
