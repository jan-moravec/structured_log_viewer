#include "loglib/internal/timestamp_promotion.hpp"

#include "loglib/internal/compact_log_value.hpp"
#include "loglib/line_source.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

namespace loglib::internal
{

namespace
{

/// Lookup the compact value for @p keyId on @p line and return its
/// underlying string bytes when the value is a string-shaped tag
/// (`MmapSlice` or `OwnedString`).
///
/// `MmapSlice` is always resolved through `LineSource::ResolveMmapBytes`
/// (file sources only; stream sources never produce this tag).
///
/// `OwnedString` resolution depends on @p ownedArena:
///   * Empty (post-pipeline backfill, stream-loop): the line's
///     payloads are already relative to the source's canonical arena,
///     so we resolve via `LineSource::ResolveOwnedBytes`.
///   * Non-empty (Stage B inline promotion): payloads are relative to
///     the per-batch staging buffer the caller passes in; we read
///     directly from it because Stage C has not yet rebased them onto
///     the canonical arena.
std::optional<std::string_view> ExtractStringBytes(
    const LogLine &line, KeyId keyId, std::string_view ownedArena
) noexcept
{
    if (keyId == kInvalidKeyId)
    {
        return std::nullopt;
    }
    const auto compact = line.CompactValues();
    auto it = std::lower_bound(compact.begin(), compact.end(), keyId, [](const auto &entry, KeyId target) {
        return entry.first < target;
    });
    if (it == compact.end() || it->first != keyId)
    {
        return std::nullopt;
    }
    const CompactLogValue &value = it->second;
    LineSource *source = line.Source();
    if (value.tag == CompactTag::MmapSlice)
    {
        if (source == nullptr)
        {
            return std::nullopt;
        }
        const std::string_view bytes = source->ResolveMmapBytes(value.payload, value.aux, line.LineId());
        if (bytes.empty() && value.aux != 0)
        {
            return std::nullopt;
        }
        return bytes;
    }
    if (value.tag == CompactTag::OwnedString)
    {
        if (!ownedArena.empty())
        {
            if (static_cast<size_t>(value.payload) + value.aux > ownedArena.size())
            {
                return std::nullopt;
            }
            return std::string_view(ownedArena.data() + value.payload, value.aux);
        }
        if (source == nullptr)
        {
            return std::nullopt;
        }
        const std::string_view bytes = source->ResolveOwnedBytes(value.payload, value.aux, line.LineId());
        if (bytes.empty() && value.aux != 0)
        {
            return std::nullopt;
        }
        return bytes;
    }
    return std::nullopt;
}

} // namespace

std::vector<TimeColumnSpec> BuildTimeColumnSpecs(KeyIndex &keys, const LogConfiguration *configuration)
{
    std::vector<TimeColumnSpec> result;
    if (configuration == nullptr)
    {
        return result;
    }
    for (const LogConfiguration::Column &column : configuration->columns)
    {
        if (column.type != LogConfiguration::Type::time)
        {
            continue;
        }
        TimeColumnSpec spec;
        spec.keyIds.reserve(column.keys.size());
        for (const std::string &key : column.keys)
        {
            spec.keyIds.push_back(keys.GetOrInsert(key));
        }
        spec.parseFormats = column.parseFormats;
        spec.formatKinds.reserve(spec.parseFormats.size());
        for (const std::string &format : spec.parseFormats)
        {
            spec.formatKinds.push_back(ClassifyTimestampFormat(format));
        }
        result.push_back(std::move(spec));
    }
    return result;
}

bool PromoteLineTimestamps(
    LogLine &line,
    std::span<const TimeColumnSpec> timeColumns,
    std::vector<std::optional<LastValidTimestampParse>> &lastValid,
    std::vector<LastTimestampBytesHit> &bytesHits,
    TimestampParseScratch &tsScratch,
    std::string_view ownedArena
)
{
    bool anyPromoted = false;
    for (size_t i = 0; i < timeColumns.size(); ++i)
    {
        const TimeColumnSpec &spec = timeColumns[i];
        std::optional<LastValidTimestampParse> &lv = lastValid[i];
        LastTimestampBytesHit &bytesHit = bytesHits[i];

        const auto tryPromote =
            [&](KeyId keyId, const std::string &format, TimestampFormatKind kind, std::string_view sv) -> bool {
            if (bytesHit.valid && bytesHit.keyId == keyId && bytesHit.bytes.size() == sv.size() &&
                std::memcmp(bytesHit.bytes.data(), sv.data(), sv.size()) == 0)
            {
                line.SetValue(keyId, bytesHit.parsed);
                return true;
            }
            TimeStamp parsed;
            if (!TryParseTimestamp(sv, format, kind, tsScratch, parsed))
            {
                return false;
            }
            line.SetValue(keyId, parsed);
            bytesHit.keyId = keyId;
            bytesHit.bytes.assign(sv.data(), sv.size());
            bytesHit.parsed = parsed;
            bytesHit.valid = true;
            return true;
        };

        bool promoted = false;
        if (lv.has_value())
        {
            if (auto sv = ExtractStringBytes(line, lv->keyId, ownedArena); sv.has_value())
            {
                if (tryPromote(lv->keyId, lv->format, lv->kind, *sv))
                {
                    promoted = true;
                }
            }
        }

        if (!promoted)
        {
            for (size_t k = 0; !promoted && k < spec.keyIds.size(); ++k)
            {
                const KeyId keyId = spec.keyIds[k];
                const auto sv = ExtractStringBytes(line, keyId, ownedArena);
                if (!sv.has_value())
                {
                    continue;
                }
                for (size_t f = 0; f < spec.parseFormats.size(); ++f)
                {
                    const std::string &format = spec.parseFormats[f];
                    const TimestampFormatKind kind = spec.formatKinds[f];
                    if (tryPromote(keyId, format, kind, *sv))
                    {
                        lv = LastValidTimestampParse{keyId, format, kind};
                        promoted = true;
                        break;
                    }
                }
            }
        }

        anyPromoted = anyPromoted || promoted;
    }
    return anyPromoted;
}

} // namespace loglib::internal
