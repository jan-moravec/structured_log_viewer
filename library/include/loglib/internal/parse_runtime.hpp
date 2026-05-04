#pragma once

#include "loglib/internal/timestamp_promotion.hpp"
#include "loglib/internal/transparent_string_hash.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_line.hpp"

#include <tsl/robin_map.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace loglib::internal
{

/// Per-worker key-string -> KeyId cache. Hit: alloc-free `find`. Miss:
/// one `KeyIndex::GetOrInsert` + write-back.
struct PerWorkerKeyCache
{
    tsl::robin_map<std::string, KeyId, TransparentStringHash, TransparentStringEqual> map;
};

/// Per-worker scratch shared by the static TBB pipeline and the
/// live-tail loop: key cache + per-time-column carry-over for inline
/// timestamp promotion.
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

    /// Promote `Type::time` columns of @p line in place. @p ownedArena
    /// is the buffer backing any `OwnedString` payloads on @p line:
    /// the TBB pipeline passes its per-batch staging buffer; the
    /// live-tail loop passes empty (resolution falls through to the
    /// line's `LineSource *`).
    void PromoteTimestamps(LogLine &line, std::span<const TimeColumnSpec> timeColumns, std::string_view ownedArena)
    {
        if (timeColumns.empty())
        {
            return;
        }
        PromoteLineTimestamps(line, timeColumns, lastValidTimestamps, lastBytesHits, tsScratch, ownedArena);
    }
};

/// Adds format-specific scratch (e.g. simdjson parser + padded
/// buffer) on top of the shared base. Used by the TBB pipeline.
template <class UserState> struct WorkerScratch : WorkerScratchBase
{
    UserState user;
};

/// Look up @p key through @p cache. View bytes must outlive the cache
/// entry on miss. `cache == nullptr` falls through to `KeyIndex`.
inline KeyId InternKeyVia(std::string_view key, KeyIndex &keys, PerWorkerKeyCache *cache)
{
    if (cache == nullptr)
    {
        return keys.GetOrInsert(key);
    }
    if (auto it = cache->map.find(key); it != cache->map.end())
    {
        return it->second;
    }
    const KeyId id = keys.GetOrInsert(key);
    cache->map.emplace(std::string(key), id);
    return id;
}

} // namespace loglib::internal
