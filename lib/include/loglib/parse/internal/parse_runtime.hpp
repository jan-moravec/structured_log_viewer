/**
 * @brief Per-thread scratch state shared by the parsing pipelines.
 *
 * Each worker keeps a cache from key text to key id and the last parsed timestamp per time column, so repeated work is
 * skipped. The static pipeline and the live-stream loop share this state. This header defines it and decides, from the
 * configuration and the names of the keys, which keys a parse converts to timestamps.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/config/column.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/transparent_string_hash.hpp"
#include "loglib/time/internal/timestamp_promotion.hpp"

#include <tsl/robin_map.h>
#include <tsl/robin_set.h>

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
 * @brief The keys that a parse converts by their name, and what a worker needs to convert them.
 *
 * A key is converted by name when IsTimeKeyName accepts it and nothing else handles it: no configured column uses it,
 * and no spec was built for it up front. The plan is built once for a parse and read by all workers, which never change
 * it.
 */
struct AutomaticTimeKeys
{
    /** Keys that something else handles. A key in this set is never converted by name. */
    tsl::robin_set<std::string, TransparentStringHash, TransparentStringEqual> excluded;
    /** What every automatic key starts from: the default formats, the zone and the reference time. */
    TimeColumnSpec prototype;

    [[nodiscard]] bool Matches(std::string_view key) const
    {
        return IsTimeKeyName(key) && !excluded.contains(key);
    }
};

/**
 * @brief How a parse converts times: the specs that are known before the first line, and the plan for the keys that
 * show up later.
 *
 * The specs are the Time columns of the configuration, and the declared keys whose name marks a time. A declared key is
 * interned before the parse starts, such as a CSV header or the group of a regex, so no worker meets it.
 */
struct TimeConversion
{
    std::vector<TimeColumnSpec> specs;
    AutomaticTimeKeys automatic;
};

/**
 * @brief Builds the time conversion of a parse. Every spec reads naive timestamps in timeZone and uses referenceTime
 * for formats without a year. Keys of the Time columns are interned in keys.
 */
TimeConversion BuildTimeConversion(
    KeyIndex &keys,
    std::span<const Column> columns,
    std::span<const KeyId> declaredKeys,
    const TimeZoneContext &timeZone,
    std::chrono::system_clock::time_point referenceTime
);

/**
 * @brief The key ids of all time columns in specs, sorted and without duplicates. It is what
 * RowBatch::convertedTimeKeys starts from.
 */
std::vector<KeyId> ConvertedTimeKeys(std::span<const TimeColumnSpec> specs);

/**
 * @brief The automatic time keys that one worker met, with what it needs to convert each of them.
 *
 * A worker adds a key when its name cache meets the key for the first time, so the check of the name runs once for each
 * key and worker, and a row costs nothing extra for keys that are not times.
 */
struct WorkerAutoTime
{
    /** The plan of the parse. Null when the worker converts nothing by name. */
    const AutomaticTimeKeys *plan = nullptr;
    std::vector<TimeColumnSpec> specs;
    std::vector<std::optional<LastValidTimestampParse>> lastValid;
    std::vector<LastTimestampBytesHit> bytesHits;
    /** The key ids of specs, sorted. */
    std::vector<KeyId> keyIds;

    void Add(KeyId id)
    {
        TimeColumnSpec spec = plan->prototype;
        spec.keyIds = {id};
        specs.push_back(std::move(spec));
        lastValid.emplace_back();
        bytesHits.emplace_back();
        keyIds.insert(std::ranges::lower_bound(keyIds, id), id);
    }
};

/**
 * @brief Each worker's own map from key text to key id, and the keys it met first in the current batch.
 *
 * A hit needs no allocation, and a miss asks the shared index once and stores the answer. Every lookup also records the
 * key in firstSeen the first time it comes up in a batch, in the order the decoder met it. The static pipeline starts a
 * new batch with BeginBatch and takes the list with TakeFirstSeen. The streaming loop never starts a new batch, so it
 * sees each key once for the whole stream and drains the list as it flushes.
 */
struct WorkerKeyCache
{
    tsl::robin_map<std::string, KeyId, TransparentStringHash, TransparentStringEqual> map;
    /** The generation of a key's last sighting, indexed by key id. */
    std::vector<uint32_t> stamp;
    uint32_t generation = 1;
    /** Keys first met in the current batch, in the order they were met. */
    std::vector<KeyId> firstSeen;
    /** The keys met so far that the parse converts by name. */
    WorkerAutoTime autoTime;

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
 * @brief Scratch data that each worker keeps, used by both the static pipeline and the streaming loop. A format keeps
 * its own scratch in its decoder.
 *
 * It holds the key cache and, for each time column, what the last successful parse was, so time columns can be
 * converted while parsing.
 */
struct WorkerScratch
{
    WorkerKeyCache keyCache;
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
     * for the batch, and the streaming loop passes an empty one, because the line's source resolves them. timeColumns
     * are the specs known up front. The worker adds the time keys it met itself.
     */
    void PromoteTimestamps(LogLine &line, std::span<const TimeColumnSpec> timeColumns, std::string_view ownedArena)
    {
        if (!timeColumns.empty())
        {
            PromoteLineTimestamps(line, timeColumns, lastValidTimestamps, lastBytesHits, tsScratch, ownedArena);
        }
        WorkerAutoTime &automatic = keyCache.autoTime;
        if (!automatic.specs.empty())
        {
            PromoteLineTimestamps(
                line, automatic.specs, automatic.lastValid, automatic.bytesHits, tsScratch, ownedArena
            );
        }
    }
};

/**
 * @brief Returns the id of a key through the worker's cache and records the key as seen. On a miss the key's bytes are
 * copied into the cache.
 */
inline KeyId InternKeyVia(std::string_view key, KeyIndex &keys, WorkerKeyCache &cache)
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
        if (cache.autoTime.plan != nullptr && cache.autoTime.plan->Matches(key))
        {
            cache.autoTime.Add(id);
        }
    }
    cache.NoteSeen(id);
    return id;
}

} // namespace loglib::internal
