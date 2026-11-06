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

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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
 * @brief Each worker's own map from key text to key id, and the keys it met first in the current batch.
 *
 * A hit needs no allocation, and a miss asks the shared index once and stores the answer. Every lookup also records the
 * key in firstSeen the first time it comes up in a batch, in the order the decoder met it. The static pipeline starts a
 * new batch with BeginBatch and takes the list with TakeFirstSeen. The streaming loop never starts a new batch, so it
 * sees each key once for the whole stream and drains the list as it flushes.
 */
struct PerWorkerKeyCache
{
    tsl::robin_map<std::string, KeyId, TransparentStringHash, TransparentStringEqual> map;
    /** The generation of a key's last sighting, indexed by key id. */
    std::vector<uint32_t> stamp;
    uint32_t generation = 1;
    /** Keys first met in the current batch, in the order they were met. */
    std::vector<KeyId> firstSeen;

    /** Forgets which keys were met, so the next lookups record them again. */
    void BeginBatch()
    {
        ++generation;
        firstSeen.clear();
    }

    /** Returns the keys first met since the last call, and starts the list again. */
    [[nodiscard]] std::vector<KeyId> TakeFirstSeen()
    {
        return std::exchange(firstSeen, {});
    }

    void NoteSeen(KeyId id)
    {
        if (id >= stamp.size())
        {
            stamp.resize(std::max<size_t>(static_cast<size_t>(id) + 1, stamp.size() * 2), 0);
        }
        if (stamp[id] != generation)
        {
            stamp[id] = generation;
            firstSeen.push_back(id);
        }
    }
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
 * @brief Returns the id of a key through the worker's cache and records the key as seen. On a miss the key's bytes are
 * copied into the cache.
 */
inline KeyId InternKeyVia(std::string_view key, KeyIndex &keys, PerWorkerKeyCache &cache)
{
    KeyId id = INVALID_KEY_ID;
    if (const auto it = cache.map.find(key); it != cache.map.end())
    {
        id = it->second;
    }
    else
    {
        id = keys.GetOrInsert(key);
        cache.map.emplace(std::string(key), id);
    }
    cache.NoteSeen(id);
    return id;
}

} // namespace loglib::internal
