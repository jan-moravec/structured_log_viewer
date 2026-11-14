#include "loglib/table/internal/enum_inference.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/line_source.hpp"
#include "loglib/core/log_data.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/schema/log_configuration.hpp"

#include <oneapi/tbb/parallel_for.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

/** Ranges shorter than this are encoded on the calling thread, where a parallel loop would cost more than it saves. */
constexpr size_t ENCODE_PARALLEL_MIN_ROWS = 4096;

/** Rows each task of the parallel lookup handles. */
constexpr size_t ENCODE_CHUNK_ROWS = 1024;

/** What one chunk of the parallel lookup found: the health counts, and the rows that need a new dictionary value. */
struct EncodeChunk
{
    size_t totalSlots = 0;
    size_t longValueSlots = 0;
    size_t wrongTypeSlots = 0;
    std::vector<size_t> pendingRows;
};

} // namespace

bool EnumInference::EncodeColumnRange(
    LogData &data, std::span<const KeyId> aliasKeys, size_t rowBegin, size_t rowEnd, EnumColumnHealth &health
)
{
    if (aliasKeys.empty())
    {
        return true;
    }
    EnumDictionary *dict = nullptr;
    {
        EnumDictionary &ref = mDictionaries.GetOrInsert(aliasKeys.front(), mValueCap);
        dict = &ref;
    }
    const auto lines = data.Lines();
    rowEnd = std::min(rowEnd, lines.size());
    const size_t valueMaxLen = mValueMaxLen;
    bool overflowed = false;

    // Encodes one row, adding its value to the dictionary when it is new.
    const auto encodeRow = [&](size_t row) {
        LogLine &line = lines[row];
        bool encoded = false;
        bool sawLong = false;
        bool sawWrongType = false;
        bool alreadyEncoded = false;
        for (const KeyId id : aliasKeys)
        {
            CompactLogValue *slot = line.FindCompactMutable(id);
            if (slot == nullptr)
            {
                continue;
            }
            if (slot->tag == CompactTag::DictRef)
            {
                alreadyEncoded = true;
                break;
            }
            const auto bytes = line.PeekStringView(*slot);
            if (!bytes.has_value())
            {
                sawWrongType = true;
                continue;
            }
            if (valueMaxLen != 0 && bytes->size() > valueMaxLen)
            {
                sawLong = true;
                continue;
            }
            const EnumValueId vid = dict->Insert(*bytes);
            if (vid == INVALID_ENUM_VALUE_ID)
            {
                // A new value does not fit. Later rows may still hold values the dictionary knows.
                overflowed = true;
                break;
            }
            *slot = CompactLogValue::MakeDictRef(vid).WithKey(id);
            encoded = true;
            break;
        }
        if (alreadyEncoded)
        {
            return;
        }
        if (encoded)
        {
            ++health.totalSlots;
        }
        else if (sawLong)
        {
            ++health.totalSlots;
            ++health.longValueSlots;
        }
        else if (sawWrongType)
        {
            ++health.totalSlots;
            ++health.wrongTypeSlots;
        }
    };

    if (rowEnd <= rowBegin || rowEnd - rowBegin < ENCODE_PARALLEL_MIN_ROWS)
    {
        for (size_t row = rowBegin; row < rowEnd; ++row)
        {
            encodeRow(row);
        }
        return !overflowed;
    }

    // Rows whose value the dictionary knows are encoded in parallel, since reading the dictionary is safe while nobody
    // writes it. A row that brings a new value is left untouched and encoded afterwards in row order, so the value ids
    // are the same as when every row is encoded in order.
    const EnumDictionary &knownValues = *dict;
    const size_t chunkCount = (rowEnd - rowBegin + ENCODE_CHUNK_ROWS - 1) / ENCODE_CHUNK_ROWS;
    std::vector<EncodeChunk> chunks(chunkCount);
    oneapi::tbb::parallel_for(size_t{0}, chunkCount, [&](size_t chunkIndex) {
        EncodeChunk &chunk = chunks[chunkIndex];
        const size_t chunkBegin = rowBegin + (chunkIndex * ENCODE_CHUNK_ROWS);
        const size_t chunkEnd = std::min(chunkBegin + ENCODE_CHUNK_ROWS, rowEnd);
        // Runs of equal values are common, so the last answer is tried before the dictionary.
        std::string_view lastBytes;
        EnumValueId lastId = INVALID_ENUM_VALUE_ID;

        // Returns false when the row needs a new dictionary value and was left untouched.
        const auto encodeKnown = [&](LogLine &line) {
            bool encoded = false;
            bool sawLong = false;
            bool sawWrongType = false;
            for (const KeyId id : aliasKeys)
            {
                CompactLogValue *slot = line.FindCompactMutable(id);
                if (slot == nullptr)
                {
                    continue;
                }
                if (slot->tag == CompactTag::DictRef)
                {
                    return true;
                }
                const auto bytes = line.PeekStringView(*slot);
                if (!bytes.has_value())
                {
                    sawWrongType = true;
                    continue;
                }
                if (valueMaxLen != 0 && bytes->size() > valueMaxLen)
                {
                    sawLong = true;
                    continue;
                }
                EnumValueId valueId = lastId;
                if (valueId == INVALID_ENUM_VALUE_ID || *bytes != lastBytes)
                {
                    valueId = knownValues.Find(*bytes);
                    if (valueId == INVALID_ENUM_VALUE_ID)
                    {
                        return false;
                    }
                    lastBytes = *bytes;
                    lastId = valueId;
                }
                *slot = CompactLogValue::MakeDictRef(valueId).WithKey(id);
                encoded = true;
                break;
            }
            if (encoded)
            {
                ++chunk.totalSlots;
            }
            else if (sawLong)
            {
                ++chunk.totalSlots;
                ++chunk.longValueSlots;
            }
            else if (sawWrongType)
            {
                ++chunk.totalSlots;
                ++chunk.wrongTypeSlots;
            }
            return true;
        };

        for (size_t row = chunkBegin; row < chunkEnd; ++row)
        {
            if (!encodeKnown(lines[row]))
            {
                chunk.pendingRows.push_back(row);
            }
        }
    });

    for (const EncodeChunk &chunk : chunks)
    {
        health.totalSlots += chunk.totalSlots;
        health.longValueSlots += chunk.longValueSlots;
        health.wrongTypeSlots += chunk.wrongTypeSlots;
        for (const size_t row : chunk.pendingRows)
        {
            encodeRow(row);
        }
    }
    return !overflowed;
}

bool EnumInference::EncodeColumnRangeAsEnum(
    LogData &data, const Column &column, size_t rowBegin, size_t rowEnd, EnumColumnHealth &health
)
{
    std::vector<KeyId> keyIds;
    keyIds.reserve(column.keys.size());
    for (const std::string &key : column.keys)
    {
        const KeyId id = data.Keys().Find(key);
        if (id != INVALID_KEY_ID)
        {
            keyIds.push_back(id);
        }
    }
    if (keyIds.empty())
    {
        return true;
    }
    return EncodeColumnRange(data, keyIds, rowBegin, rowEnd, health);
}

void EnumInference::PromoteColumnToEnum(LogData &data, LogConfigurationManager &configuration, size_t columnIndex)
{
    if (columnIndex >= configuration.Configuration().columns.size())
    {
        return;
    }
    {
        const auto &snapshot = configuration.Configuration().columns[columnIndex];
        if (snapshot.type == ColumnType::Enumeration || snapshot.type == ColumnType::Level)
        {
            return;
        }
    }

    const std::vector<std::string> columnKeys = configuration.Configuration().columns[columnIndex].keys;
    const std::string headerKey = configuration.Configuration().columns[columnIndex].header;

    configuration.SetColumnType(columnIndex, ColumnType::Enumeration);
    KeyId canonicalKey = INVALID_KEY_ID;
    {
        std::optional<KeyId> canonical;
        for (const std::string &key : columnKeys)
        {
            const KeyId id = data.Keys().GetOrInsert(key);
            if (!canonical.has_value())
            {
                (void)mDictionaries.GetOrInsert(id, mValueCap);
                canonical = id;
            }
            else
            {
                if (!mDictionaries.Alias(*canonical, id))
                {
                    ReportAliasFailure(data.Keys(), *canonical, id, headerKey);
                }
            }
        }
        if (canonical.has_value())
        {
            canonicalKey = *canonical;
        }
    }

    EnumColumnHealth &health = mColumnHealth[canonicalKey];
    if (!EncodeColumnRangeAsEnum(
            data, configuration.Configuration().columns[columnIndex], 0U, data.Lines().size(), health
        ))
    {
        DemoteColumnFromEnum(data, configuration, columnIndex);
        return;
    }
    if (health.ShouldDemote(ENUM_HEALTH_TOLERANCE_RATIO, ENUM_HEALTH_MIN_SAMPLES))
    {
        DemoteColumnFromEnum(data, configuration, columnIndex);
        return;
    }

    if (std::ranges::any_of(columnKeys, IsLogLevelKey))
    {
        MaybePromoteToLevel(data, configuration, columnIndex);
    }
}

void EnumInference::DemoteColumnFromEnum(
    LogData &data, LogConfigurationManager &configuration, size_t columnIndex, bool recordForBatch
)
{
    const auto &columns = configuration.Configuration().columns;
    if (columnIndex >= columns.size())
    {
        return;
    }
    const auto &column = columns[columnIndex];
    if (column.type != ColumnType::Enumeration && column.type != ColumnType::Level)
    {
        return;
    }

    std::vector<KeyId> keyIds;
    keyIds.reserve(column.keys.size());
    for (const std::string &key : column.keys)
    {
        const KeyId id = data.Keys().Find(key);
        if (id != INVALID_KEY_ID)
        {
            keyIds.push_back(id);
        }
    }

    if (recordForBatch && !keyIds.empty())
    {
        mLastBatchDemotedKeys.push_back(keyIds.front());
    }

    const auto lines = data.Lines();

    if (!keyIds.empty())
    {
        const EnumDictionary *dict = mDictionaries.Find(keyIds.front());
        if (column.type == ColumnType::Level && dict != nullptr)
        {
            if (const auto ranksIt = mLevelRankCache.find(keyIds.front()); ranksIt != mLevelRankCache.end())
            {
                LevelValueMap values;
                const std::vector<LogLevel> &ranks = ranksIt->second;
                const size_t known = std::min<size_t>(ranks.size(), dict->Size());
                for (size_t valueId = 0; valueId < known; ++valueId)
                {
                    if (ranks[valueId] != LogLevel::Unknown)
                    {
                        values[ranks[valueId]].emplace_back(dict->Resolve(static_cast<EnumValueId>(valueId)));
                    }
                }
                if (!values.empty())
                {
                    mDemotedLevelValues[keyIds.front()] = std::move(values);
                }
            }
        }
        for (auto &line : lines)
        {
            LineSource *source = line.Source();
            const size_t lineId = line.LineId();
            for (const KeyId id : keyIds)
            {
                CompactLogValue *slot = line.FindCompactMutable(id);
                if (slot == nullptr || slot->tag != CompactTag::DictRef)
                {
                    continue;
                }
                std::string_view bytes;
                if (dict != nullptr)
                {
                    bytes = dict->Resolve(static_cast<EnumValueId>(slot->payload));
                }
                if (source != nullptr)
                {
                    const uint64_t offset = source->AppendOwnedBytes(lineId, bytes);
                    *slot = CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(bytes.size())).WithKey(id);
                }
                else
                {
                    *slot = CompactLogValue::MakeMonostate().WithKey(id);
                }
            }
        }
        mDictionaries.Erase(keyIds.front());
    }

    configuration.SetColumnType(columnIndex, ColumnType::String);

    if (!keyIds.empty())
    {
        mColumnHealth.erase(keyIds.front());
        mLevelRankCache.erase(keyIds.front());
    }
}

} // namespace loglib
