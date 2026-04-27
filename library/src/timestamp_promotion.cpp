#include "timestamp_promotion.hpp"

#include <cstring>
#include <utility>

namespace loglib::detail
{

std::vector<TimeColumnSpec>
BuildTimeColumnSpecs(KeyIndex &keys, const LogConfiguration *configuration)
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
    TimestampParseScratch &tsScratch
)
{
    bool anyPromoted = false;
    for (size_t i = 0; i < timeColumns.size(); ++i)
    {
        const TimeColumnSpec &spec = timeColumns[i];
        std::optional<LastValidTimestampParse> &lv = lastValid[i];
        LastTimestampBytesHit &bytesHit = bytesHits[i];

        const auto getStringFor = [&line](KeyId keyId) -> std::optional<std::string_view> {
            if (keyId == kInvalidKeyId)
            {
                return std::nullopt;
            }
            LogValue value = line.GetValue(keyId);
            return AsStringView(value);
        };

        const auto tryPromote = [&](KeyId keyId, const std::string &format,
                                    TimestampFormatKind kind, std::string_view sv) -> bool {
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
            if (auto sv = getStringFor(lv->keyId); sv.has_value())
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
                auto sv = getStringFor(keyId);
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

} // namespace loglib::detail
