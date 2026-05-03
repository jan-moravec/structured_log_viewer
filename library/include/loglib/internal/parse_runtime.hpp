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

/// Per-worker key string -> KeyId cache. Hit: `find(string_view)` is alloc-free;
/// miss: one `KeyIndex::GetOrInsert` plus a write-back.
struct PerWorkerKeyCache
{
    tsl::robin_map<std::string, KeyId, TransparentStringHash, TransparentStringEqual> map;
};

/// Per-worker scratch shared across parsers: key cache plus per-time-column
/// carry-over for the inline timestamp-promotion hook. Used by both the
/// static TBB pipeline (`RunStaticParserPipeline`) and the live-tail loop
/// (`RunStreamingParseLoop`).
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

    /// Promote the configured `Type::time` columns of @p line in place.
    /// @p ownedArena is the byte buffer that any `OwnedString` compact
    /// values on @p line currently reference: for the TBB pipeline that
    /// is the per-batch staging buffer (`ParsedPipelineBatch::ownedStringsArena`);
    /// for the live-tail loop the source already owns the bytes, so an
    /// empty view is passed and resolution falls through to the
    /// `LineSource *` on the line.
    void PromoteTimestamps(LogLine &line, std::span<const TimeColumnSpec> timeColumns, std::string_view ownedArena)
    {
        if (timeColumns.empty())
        {
            return;
        }
        PromoteLineTimestamps(line, timeColumns, lastValidTimestamps, lastBytesHits, tsScratch, ownedArena);
    }
};

/// Bolts format-specific scratch (e.g. simdjson parser + padded buffer) onto
/// the shared base. Used by the TBB pipeline; the live-tail loop passes
/// `WorkerScratchBase` directly.
template <class UserState> struct WorkerScratch : WorkerScratchBase
{
    UserState user;
};

/// Routes a key lookup through the per-worker cache. The view's bytes must
/// outlive the cache entry on the miss path. Passing `cache == nullptr`
/// falls back to a direct `KeyIndex::GetOrInsert` call.
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
