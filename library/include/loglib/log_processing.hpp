#pragma once

#include "key_index.hpp"
#include "log_configuration.hpp"
#include "log_line.hpp"

#include <date/tz.h>

#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace loglib
{

/**
 * @brief Per-line carry-over for `ParseTimestampLine`'s "remember the last
 *        successful (keyId, format) pair" fast path.
 *
 * The legacy `ParseTimestamps` whole-data pass and the streaming Stage B
 * in-pipeline promotion path (PRD §4.2a / req. 4.2.21) both walk a list of
 * lines and try a (keyIds × parseFormats) matrix per line. In real log
 * files a single (keyId, format) pair almost always succeeds for every
 * line in a column, so caching the last winner here means subsequent lines
 * pay one `date::parse` call instead of |keyIds| × |parseFormats|.
 *
 * The cache is per-(walk over a column's lines) for the legacy path and
 * per-(worker × time column index) for the streaming path. Resetting at
 * the start of a walk matches the legacy semantics exactly; streaming
 * carries one of these per Stage B `WorkerState`'s per-time-column slot
 * across batches because a single worker walks the same set of columns
 * over and over.
 */
struct LastValidTimestampParse
{
    KeyId keyId = kInvalidKeyId;
    std::string format;
};

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
 * @brief Parses the timestamps for a single configured `Type::time` column over
 *        an arbitrary slice of `LogLine`s.
 *
 * Extracted from the per-column inner loop of `ParseTimestamps` so the
 * streaming back-fill (`LogTable::AppendBatch`, PRD req. 4.1.13b) and the
 * legacy whole-data pass can share a single implementation. Walks @p lines in
 * order, applying the column's `keys × parseFormats` matrix to each line and
 * promoting matched string values to `TimeStamp` via `LogLine::SetValue`.
 * Reuses the `LastValidTimestampParse` fast path from the legacy pass so files
 * that use one timestamp format throughout pay one strptime call per line.
 *
 * @param column Configured `Type::time` column. Caller must ensure
 *               `column.type == LogConfiguration::Type::time`; the function
 *               does not re-check.
 * @param lines  Mutable slice of lines to back-fill in place.
 * @return Per-line failure messages (one per line that did not match any
 *         (key, format) pair). Empty if every line parsed.
 */
std::vector<std::string> BackfillTimestampColumn(const LogConfiguration::Column &column, std::vector<LogLine> &lines);

/**
 * @brief Promotes one log line's timestamp string to a `TimeStamp` using the
 *        provided pre-resolved (KeyId, format) matrix.
 *
 * Public, header-exposed overload of the per-line back-fill body that the
 * streaming pipeline's Stage B uses to promote `Type::time` column values
 * inline (PRD req. 4.2.21 / §4.2a / parser-perf task 3.0). Caller pre-resolves
 * the column's `keys -> KeyIds` once at pipeline start (or once at
 * BackfillTimestampColumn entry) so this function does no string-keyed
 * lookups; the per-line work is `LogLine::GetValue(KeyId)` (linear scan over
 * the small sorted pair vector) plus a `date::parse` call.
 *
 * Walks the matrix in `keyIds × parseFormats` order, with the
 * `lastValid`-cached pair tried first. On a match, the line's value at the
 * matching KeyId is replaced with the parsed `TimeStamp` and `lastValid` is
 * updated. On no match, the line is left untouched (caller may subsequently
 * call again with a wider matrix or report the failure).
 *
 * Failure-mode contract (PRD §4.2a.5): when no (keyId, format) pair matches,
 * the function silently returns `false` and does not touch the line. The
 * legacy whole-data `BackfillTimestampColumn` wraps this and surfaces a
 * human-readable error per failure; the streaming Stage B does *not* — it
 * leaves the value as a string so `LogTable::AppendBatch`'s mid-stream
 * back-fill loop can take a second pass on it (matches the existing GUI-side
 * silent-discard semantics in `log_table.cpp` step 4).
 *
 * @param line          Log line to promote in place.
 * @param keyIds        Resolved KeyIds for this column's `keys` list. Entries
 *                      equal to `kInvalidKeyId` are silently skipped; the
 *                      caller does not need to filter them out.
 * @param parseFormats  Format strings to feed `date::parse`. Order matches
 *                      `LogConfiguration::Column::parseFormats` so the first
 *                      format that parses wins.
 * @param lastValid     In/out cache of the last (keyId, format) pair that
 *                      successfully promoted a line. Reset by the caller at
 *                      the start of a column walk. The function tries this
 *                      pair first; on success it stays put, on failure the
 *                      function falls back to the full matrix and updates the
 *                      cache to whichever pair won (if any).
 * @return `true` iff a (keyId, format) pair matched and the line's value at
 *         that KeyId was promoted to `TimeStamp`.
 */
bool ParseTimestampLine(
    LogLine &line,
    std::span<const KeyId> keyIds,
    std::span<const std::string> parseFormats,
    std::optional<LastValidTimestampParse> &lastValid
);

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
