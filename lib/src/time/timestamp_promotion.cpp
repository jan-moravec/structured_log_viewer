#include "loglib/time/internal/timestamp_promotion.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/line_source.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/time/user_timestamp.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <utility>

namespace loglib::internal
{

namespace
{

/**
 * @brief Returns the bytes of a string value of the line, or nothing if the key is missing or not a string.
 *
 * A file slice is resolved through the line's source. For a copied string, a non-empty ownedArena is the per-batch
 * staging buffer that the value still points into, because its offsets are not yet rebased onto the source's arena. An
 * empty ownedArena means the offsets already refer to the source, so the source resolves them.
 */
std::optional<std::string_view> ExtractStringBytes(
    const LogLine &line, KeyId keyId, std::string_view ownedArena
) noexcept
{
    if (keyId == INVALID_KEY_ID)
    {
        return std::nullopt;
    }
    const auto compact = line.CompactValues();
    // Asymmetric comparator: std::ranges::lower_bound rejects it.
    auto it = std::lower_bound(compact.begin(), compact.end(), keyId, [](const auto &entry, KeyId target) {
        return entry.first < target;
    });
    if (it == compact.end() || it->first != keyId)
    {
        return std::nullopt;
    }
    const CompactLogValue &value = it->second;
    const LineSource *source = line.Source();
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

/**
 * @brief Converts a wall clock to UTC. A zone lookup is slow, so window keeps the last offset and the full lookup runs
 * only next to a transition.
 */
std::int64_t LocalToUtc(std::int64_t localMicros, const TimeZoneContext &timeZone, LocalOffsetWindow &window)
{
    if (!window.Contains(localMicros))
    {
        window = timeZone.OffsetWindowAt(localMicros);
        if (!window.Contains(localMicros))
        {
            return timeZone.LocalMicrosecondsToUtc(localMicros);
        }
    }
    return localMicros - window.utcOffset;
}

} // namespace

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
            if (IsNaiveTimestamp(sv, format))
            {
                parsed = TimeStamp{std::chrono::microseconds{
                    LocalToUtc(parsed.time_since_epoch().count(), spec.timeZone, bytesHit.offsetWindow)
                }};
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
                        lv = LastValidTimestampParse{.keyId = keyId, .format = format, .kind = kind};
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
