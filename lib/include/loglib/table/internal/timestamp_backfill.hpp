/**
 * @brief Parses timestamp columns after the rows already exist.
 *
 * When a column becomes a time column after its rows were parsed, for example because the user changed its type, these
 * functions parse its text and replace it with timestamps. BackfillTimestampColumn does it for one column over a range
 * of rows. Text without an offset is read in
 * the given zone, and formats without a year use the reference time. The version taking BackfillErrors::Discard skips
 * building error messages, which keeps the streaming path cheap.
 *
 * This is part of the table module.
 */
#pragma once

#include "loglib/core/key_index.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/schema/log_configuration.hpp"
#include "loglib/time/time_zone_context.hpp"

#include <chrono>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace loglib
{

/**
 * @brief Converts the text of one time column into timestamps, in place.
 *
 * The column must have type Type::Time. keyIndex is the index that the key ids of the rows come from. Pass a sub-span
 * of the rows to convert only a slice, for example the rows of a streamed batch. Returns one message per value that
 * could not be parsed.
 */
std::vector<std::string> BackfillTimestampColumn(
    const KeyIndex &keyIndex,
    const Column &column,
    std::span<LogLine> lines,
    const TimeZoneContext &timeZone,
    std::chrono::system_clock::time_point referenceTime
);

/** @brief Selects the overload that does not build error messages, which saves time when streaming. */
enum class BackfillErrors : uint8_t
{
    Discard,
};

/** @brief Same as BackfillTimestampColumn, but does not report which values failed to parse. */
void BackfillTimestampColumn(
    const KeyIndex &keyIndex,
    const Column &column,
    std::span<LogLine> lines,
    const TimeZoneContext &timeZone,
    std::chrono::system_clock::time_point referenceTime,
    BackfillErrors discardErrors
);

} // namespace loglib
