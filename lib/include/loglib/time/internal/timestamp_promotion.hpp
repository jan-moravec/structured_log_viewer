/**
 * @brief Converts timestamp text in a row to real timestamps.
 *
 * A TimeColumnSpec describes one Time column: its key ids, its formats, the zone for values that name none, and the
 * reference time for formats without a year. The parsers and the table use PromoteLineTimestamps to parse that text
 * with the configured formats and replace it with a timestamp. The helpers remember the last successful format and text
 * per column to skip work.
 *
 * This is part of the time module. It needs only LogLine and the timestamp parsing functions, so building the specs
 * from a configuration is left to the callers.
 */
#pragma once

#include "loglib/core/key_index.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/time/time_zone_context.hpp"
#include "loglib/time/timestamp_parsing.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace loglib::internal
{

/**
 * @brief The last timestamp text a worker parsed for a time column and its result. Consecutive lines often share a
 * timestamp, so equal text skips parsing.
 */
struct LastTimestampBytesHit
{
    KeyId keyId = INVALID_KEY_ID;
    std::string bytes;
    TimeStamp parsed;
    bool valid = false;
    /** @brief The offset window last used for a value that names no zone. */
    LocalOffsetWindow offsetWindow;
};

/** @brief A Time column prepared for parsing: its keys as ids and its formats already prepared. */
struct TimeColumnSpec
{
    std::vector<KeyId> keyIds;
    std::vector<TimestampFormat> formats;
    /** @brief The zone in which values that name no zone are read. */
    TimeZoneContext timeZone;
    /** @brief The "now" for formats that carry no year, fixed once per parse. */
    std::chrono::system_clock::time_point referenceTime;
};

/**
 * @brief Converts the time columns of one line to timestamps, in place. Returns true if at least one column was
 * converted.
 *
 * A line that matches none of the keys and formats is left alone, and LogTable converts it later. ownedArena holds the
 * bytes behind the line's OwnedString values: while parsing a batch it is the batch's staging buffer, and an empty one
 * means the line's source resolves them.
 */
bool PromoteLineTimestamps(
    LogLine &line,
    std::span<const TimeColumnSpec> timeColumns,
    std::vector<std::optional<LastValidTimestampParse>> &lastValid,
    std::vector<LastTimestampBytesHit> &bytesHits,
    TimestampParseScratch &tsScratch,
    std::string_view ownedArena
);

} // namespace loglib::internal
