#include "loglib/timestamp_backfill.hpp"

#include "loglib/internal/timestamp_promotion.hpp"
#include "loglib/timestamp_parsing.hpp"

#include <fmt/format.h>

#include <array>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>

namespace loglib
{

namespace
{

/// Builds the per-line scratch state shared by both `BackfillTimestampColumn`
/// overloads. Returns `false` when @p lines is empty, in which case the
/// caller should bail out (the spec arrays are not built).
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

} // namespace

namespace
{

/// Returns an empty view: `BackfillTimestampColumn` over a `LogLine`
/// span flows through `PromoteLineTimestamps`, which now resolves the
/// per-line owned-bytes arena via the line's `LineSource` directly
/// (`source->ResolveOwnedBytes(offset, length, lineId)`). The
/// `ownedArena` parameter only matters during Stage B / Stage C of the
/// parser pipeline, where the per-batch staging buffer carries
/// `OwnedString` payloads before they are rebased onto the canonical
/// source arena. This backfill path runs after the pipeline, when
/// every payload is already source-relative.
std::string_view OwnedArenaForBackfill(const LogLine & /*line*/) noexcept
{
    return std::string_view{};
}

} // namespace

std::vector<std::string> BackfillTimestampColumn(const Column &column, std::span<LogLine> lines)
{
    std::vector<std::string> errors;
    std::array<internal::TimeColumnSpec, 1> specs;
    std::vector<std::optional<LastValidTimestampParse>> lastValid;
    std::vector<internal::LastTimestampBytesHit> bytesHits;
    if (!MakeBackfillState(column, lines, specs, lastValid, bytesHits))
    {
        return errors;
    }

    TimestampParseScratch scratch;
    for (auto &line : lines)
    {
        const std::string_view ownedArena = OwnedArenaForBackfill(line);
        if (!internal::PromoteLineTimestamps(line, specs, lastValid, bytesHits, scratch, ownedArena))
        {
            errors.emplace_back(
                fmt::format(
                    "Failed to parse a timestamp for column '{}' from line number {}", column.header, line.LineId()
                )
            );
        }
    }
    return errors;
}

void BackfillTimestampColumn(const Column &column, std::span<LogLine> lines, BackfillErrors discardErrors)
{
    static_cast<void>(discardErrors);
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
        const std::string_view ownedArena = OwnedArenaForBackfill(line);
        static_cast<void>(internal::PromoteLineTimestamps(line, specs, lastValid, bytesHits, scratch, ownedArena));
    }
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
