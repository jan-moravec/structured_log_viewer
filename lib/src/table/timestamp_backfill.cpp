#include "loglib/table/internal/timestamp_backfill.hpp"

#include "loglib/time/internal/timestamp_promotion.hpp"
#include "loglib/time/timestamp_parsing.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace loglib
{

namespace
{

/**
 * @brief Builds the scratch state for converting one column. Returns false when lines is empty, and then nothing else
 * is set.
 */
bool MakeBackfillState(
    const KeyIndex &keyIndex,
    const Column &column,
    std::span<LogLine> lines,
    const TimeZoneContext &timeZone,
    std::chrono::system_clock::time_point referenceTime,
    std::array<internal::TimeColumnSpec, 1> &specsOut,
    std::vector<std::optional<LastValidTimestampParse>> &lastValidOut,
    std::vector<internal::LastTimestampBytesHit> &bytesHitsOut
)
{
    if (lines.empty())
    {
        return false;
    }

    internal::TimeColumnSpec spec;
    spec.keyIds.reserve(column.keys.size());
    for (const std::string &key : column.keys)
    {
        spec.keyIds.push_back(keyIndex.Find(key));
    }
    spec.formats.reserve(column.parseFormats.size());
    for (const std::string &format : column.parseFormats)
    {
        spec.formats.emplace_back(format);
    }
    spec.timeZone = timeZone;
    spec.referenceTime = referenceTime;
    specsOut[0] = std::move(spec);
    lastValidOut.assign(1, std::nullopt);
    bytesHitsOut.assign(1, internal::LastTimestampBytesHit{});
    return true;
}

/**
 * @brief Converts the column in every line. When errors is not null, adds one message per line that failed.
 *
 * The lines are already resolved through their own source, so the owned-bytes view passed to PromoteLineTimestamps is
 * empty.
 */
void BackfillColumnLoop(
    const KeyIndex &keyIndex,
    const Column &column,
    std::span<LogLine> lines,
    const TimeZoneContext &timeZone,
    std::chrono::system_clock::time_point referenceTime,
    std::vector<std::string> *errors
)
{
    std::array<internal::TimeColumnSpec, 1> specs;
    std::vector<std::optional<LastValidTimestampParse>> lastValid;
    std::vector<internal::LastTimestampBytesHit> bytesHits;
    if (!MakeBackfillState(keyIndex, column, lines, timeZone, referenceTime, specs, lastValid, bytesHits))
    {
        return;
    }

    TimestampParseScratch scratch;
    for (auto &line : lines)
    {
        if (!internal::PromoteLineTimestamps(line, specs, lastValid, bytesHits, scratch, std::string_view{}) &&
            errors != nullptr)
        {
            errors->emplace_back(
                fmt::format(
                    "Failed to parse a timestamp for column '{}' from line number {}", column.header, line.LineId()
                )
            );
        }
    }
}

} // namespace

std::vector<std::string> BackfillTimestampColumn(
    const KeyIndex &keyIndex,
    const Column &column,
    std::span<LogLine> lines,
    const TimeZoneContext &timeZone,
    std::chrono::system_clock::time_point referenceTime
)
{
    std::vector<std::string> errors;
    BackfillColumnLoop(keyIndex, column, lines, timeZone, referenceTime, &errors);
    return errors;
}

void BackfillTimestampColumn(
    const KeyIndex &keyIndex,
    const Column &column,
    std::span<LogLine> lines,
    const TimeZoneContext &timeZone,
    std::chrono::system_clock::time_point referenceTime,
    BackfillErrors discardErrors
)
{
    static_cast<void>(discardErrors);
    BackfillColumnLoop(keyIndex, column, lines, timeZone, referenceTime, nullptr);
}

} // namespace loglib
