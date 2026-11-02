/**
 * @brief Converts timestamp text in a row to real timestamps while parsing.
 *
 * When the configuration marks a column as time, the parsers use these helpers to parse its text with the configured
 * formats and replace it with a timestamp. The helpers remember the last successful format per column to skip work.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/key_index.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/schema/log_configuration.hpp"
#include "loglib/time/time_zone_context.hpp"
#include "loglib/time/timestamp_parsing.hpp"

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace loglib::internal
{

/**
 * Per-worker per-time-column same-bytes short-circuit, exploiting the common
 * case of consecutive lines sharing a timestamp.
 */
struct LastTimestampBytesHit
{
    KeyId keyId = INVALID_KEY_ID;
    std::string bytes;
    TimeStamp parsed{};
    bool valid = false;
    /** Last zone offset used for a value with no zone of its own. */
    LocalOffsetWindow offsetWindow;
};

/**
 * Pre-resolved view of one configured Type::Time column: keys resolved to
 * KeyIds, formats pre-classified.
 */
struct TimeColumnSpec
{
    std::vector<KeyId> keyIds;
    std::vector<std::string> parseFormats;
    std::vector<TimestampFormatKind> formatKinds;
    /** Zone in which values with no zone of their own are read. */
    TimeZoneContext timeZone;
};

/**
 * Builds one TimeColumnSpec per Type::Time column. Returns empty when
 * @p configuration is null.
 */
std::vector<TimeColumnSpec> BuildTimeColumnSpecs(KeyIndex &keys, const LogConfiguration *configuration);

/**
 * Promotes one line's Type::Time columns in place. Returns true iff at
 * least one column was promoted on this line. Lines that don't match any
 * (KeyId, format) pair are left untouched — the LogTable mid-stream
 * back-fill picks them up.
 *
 * @p ownedArena is the byte buffer that any OwnedString compact values
 * on @p line currently reference: during Stage B that's the per-batch
 * staging buffer (ParsedPipelineBatch::ownedStringsArena); empty means
 * the payloads resolve through the line's LineSource.
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
