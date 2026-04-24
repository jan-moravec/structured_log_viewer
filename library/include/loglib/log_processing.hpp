#pragma once

#include "log_configuration.hpp"
#include "log_line.hpp"

#include <date/tz.h>

#include <filesystem>

namespace loglib
{

/**
 * @brief Initializes the log processing library with timezone data.
 *
 * This function must be called before any other log processing functions to ensure
 * proper timezone handling during timestamp conversions.
 *
 * @param tzdata Path to the timezone database directory.
 */
void Initialize(const std::filesystem::path &tzdata);

/**
 * @brief Returns the process-wide cached pointer to the current IANA time zone.
 *
 * The returned pointer is lazily initialized on the first call; it must not be invoked
 * before ::loglib::Initialize has installed the tzdata database. All formatters and
 * converters in the library route their zone lookups through this function so they
 * share a single zone instance.
 *
 * @return Non-owning pointer to the current time zone (never null after successful
 *         initialization).
 */
const date::time_zone *CurrentZone();

/**
 * @brief Parses timestamps from log data according to the provided configuration.
 *
 * Processes timestamp information from the log data based on the
 * timestamp format and other settings specified in the configuration.
 * Replaces the original timestamp values in the log data with the parsed timestamps.
 *
 * @param logData Reference to the log data to be processed.
 * @param configuration Configuration settings that define how timestamps should be parsed.
 * @return std::vector<std::string> Human-readable error messages for log lines whose
 *         configured timestamp column could not be parsed. Empty if all timestamps parsed successfully.
 */
std::vector<std::string> ParseTimestamps(LogData &logData, const LogConfiguration &configuration);

/**
 * @brief Converts a TimeStamp object to local time in milliseconds since epoch.
 *
 * @param timeStamp The TimeStamp object to convert.
 * @return int64_t The number of milliseconds since epoch in local timezone.
 */
int64_t TimeStampToLocalMillisecondsSinceEpoch(TimeStamp timeStamp);

/**
 * @brief Converts UTC time in microseconds to local time in milliseconds.
 *
 * @param microseconds The number of microseconds since epoch in UTC.
 * @return int64_t The number of milliseconds since epoch in local timezone.
 */
int64_t UtcMicrosecondsToLocalMilliseconds(int64_t microseconds);

/**
 * @brief Converts local time in milliseconds since epoch to a TimeStamp object.
 *
 * @param milliseconds The number of milliseconds since epoch in local timezone.
 * @return TimeStamp A TimeStamp object representing the specified time.
 */
TimeStamp LocalMillisecondsSinceEpochToTimeStamp(int64_t milliseconds);

/**
 * @brief Formats UTC time in microseconds as a human-readable date-time string.
 *
 * @param microseconds The number of microseconds since epoch in UTC.
 * @return std::string A formatted date-time string.
 */
std::string UtcMicrosecondsToDateTimeString(int64_t microseconds);

/**
 * @brief Converts a TimeStamp object to a human-readable date-time string.
 *
 * @param timeStamp The TimeStamp object to convert.
 * @return std::string A formatted date-time string.
 */
std::string TimeStampToDateTimeString(TimeStamp timeStamp);

} // namespace loglib
