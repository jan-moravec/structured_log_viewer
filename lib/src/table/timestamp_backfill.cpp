#include "loglib/table/internal/timestamp_backfill.hpp"

#include "loglib/time/internal/timestamp_promotion.hpp"
#include "loglib/time/timestamp_parsing.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <array>
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
    const Column &column,
    std::span<LogLine> lines,
    std::array<internal::TimeColumnSpec, 1> &specsOut,
    std::vector<std::optional<LastValidTimestampParse>> &lastValidOut,
    std::vector<internal::LastTimestampBytesHit> &bytesHitsOut
)
{
    if (lines.empty())
    {
        return false;
    }

    const KeyIndex &keyIndex = lines.front().Keys();
    internal::TimeColumnSpec spec;
    spec.keyIds.reserve(column.keys.size());
    for (const std::string &key : column.keys)
    {
        spec.keyIds.push_back(keyIndex.Find(key));
    }
    spec.parseFormats = column.parseFormats;
    spec.formatKinds.reserve(spec.parseFormats.size());
    for (const std::string &format : spec.parseFormats)
    {
        spec.formatKinds.push_back(ClassifyTimestampFormat(format));
    }
    spec.timeZone = ProcessDefaultTimeZone();
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
void BackfillColumnLoop(const Column &column, std::span<LogLine> lines, std::vector<std::string> *errors)
{
    std::array<internal::TimeColumnSpec, 1> specs;
    std::vector<std::optional<LastValidTimestampParse>> lastValid;
    std::vector<internal::LastTimestampBytesHit> bytesHits;
    if (!MakeBackfillState(column, lines, specs, lastValid, bytesHits))
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

std::vector<std::string> BackfillTimestampColumn(const Column &column, std::span<LogLine> lines)
{
    std::vector<std::string> errors;
    BackfillColumnLoop(column, lines, &errors);
    return errors;
}

void BackfillTimestampColumn(const Column &column, std::span<LogLine> lines, BackfillErrors discardErrors)
{
    static_cast<void>(discardErrors);
    BackfillColumnLoop(column, lines, nullptr);
}

std::vector<std::string> ParseTimestamps(LogData &logData, const LogConfiguration &configuration)
{
    std::vector<std::string> errors;

    for (const auto &column : configuration.columns)
    {
        if (column.type == ColumnType::Time)
        {
            auto columnErrors = BackfillTimestampColumn(column, logData.Lines());
            if (!columnErrors.empty())
            {
                errors.reserve(errors.size() + columnErrors.size());
                std::ranges::move(columnErrors, std::back_inserter(errors));
            }
        }
    }

    return errors;
}

} // namespace loglib
