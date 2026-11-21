/**
 * @brief The generic date::parse reader for time formats.
 *
 * TryParseGenericTimestamp reads a time with date::parse and any format. TimestampFormat uses hand-written parsers
 * for the formats it knows and goes through the same date::parse calls for the rest, so this function is the slow
 * reference that tests, benchmarks and the fuzz target compare the faster readers with.
 *
 * This is part of the time module.
 */
#pragma once

#include "loglib/core/log_value.hpp"
#include "loglib/time/internal/timestamp_parsing.hpp"

#include <string>
#include <string_view>

namespace loglib::internal
{

/**
 * @brief Parses with date::parse and an arbitrary format. It succeeds only if the format uses all of the text. The
 * scratch buffers are reused across calls.
 */
bool TryParseGenericTimestamp(
    std::string_view sv, const std::string &format, TimestampParseScratch &scratch, TimeStamp &out
);

} // namespace loglib::internal
