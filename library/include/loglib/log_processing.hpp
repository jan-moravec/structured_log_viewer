#pragma once

#include "key_index.hpp"
#include "log_configuration.hpp"
#include "log_line.hpp"

#include <date/tz.h>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace loglib
{

/**
 * @brief Coarse classification of timestamp `parseFormats` strings used to
 *        dispatch the per-line parse to a hand-rolled fast path when the
 *        format matches one of the common ISO-8601 shapes.
 *
 * `Generic` always falls through to the existing `date::parse` /
 * `std::istringstream` path, so adding a new fast-path kind is strictly
 * additive — unknown formats keep the legacy semantics.
 *
 * The `_T` / `_Space` variants differ only in the date/time separator
 * character (`'T'` vs `' '`), matching `%FT%T` and `%F %T` respectively.
 * Both accept an optional `.fff` / `.ffffff` fractional-seconds suffix
 * (millisecond / microsecond precision), which `%T` does too in Howard
 * Hinnant's parser. Anything trailing past microsecond precision (e.g.
 * timezone offsets, sub-microsecond fractions) round-trips back to the
 * generic path.
 */
enum class TimestampFormatKind : std::uint8_t
{
    Generic,
    Iso8601_T,
    Iso8601_Space,
};

/**
 * @brief Returns the fast-path kind for @p format, or `Generic` for any
 *        format string that does not match one of the recognised ISO-8601
 *        shapes verbatim.
 *
 * The recognised set is intentionally conservative — only `"%FT%T"` and
 * `"%F %T"`. Any decoration such as a trailing `%Ez` (timezone offset),
 * leading whitespace, or alternative date-component ordering routes the
 * caller back to `date::parse`.
 *
 * Pure function; cheap enough to call once per (column, format) at config
 * time so the streaming Stage B hot path never re-classifies per line.
 */
TimestampFormatKind ClassifyTimestampFormat(std::string_view format);

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
 *
 * `kind` is the cached result of `ClassifyTimestampFormat(format)` so the
 * hot path can dispatch the fast/slow-path branch on a single byte
 * comparison rather than re-classifying the format string per line.
 */
struct LastValidTimestampParse
{
    KeyId keyId = kInvalidKeyId;
    std::string format;
    TimestampFormatKind kind = TimestampFormatKind::Generic;
};

/**
 * @brief Per-caller scratch buffers for the generic `date::parse` fallback.
 *
 * The legacy slow path constructed a fresh `std::istringstream` and copied
 * the timestamp bytes into a fresh `std::string` on every call — two heap
 * allocations per line on the [stream_to_table] hot path. Threading one of
 * these through the per-line helpers lets the streaming Stage B amortise
 * both allocations across an entire batch (one per worker, lifetime tied
 * to `WorkerState`) and lets the legacy whole-data path own one scratch
 * for the duration of a column walk.
 *
 * The struct is intentionally trivial — callers `clear()`/`assign()` the
 * fields directly through the helper functions below; nothing else touches
 * them.
 */
struct TimestampParseScratch
{
    std::string str;
    std::istringstream stream;
};

/**
 * @brief Parses an ISO-8601-style timestamp directly from @p sv into @p out.
 *
 * Recognised shape: `YYYY-MM-DD<sep>HH:MM:SS[.fff[fff]]` where `<sep>` is
 * either `'T'` or `' '` (callers pass the desired separator). Up to six
 * fractional-second digits are accepted (rounded to microsecond precision).
 * Anything else — leading whitespace, alternative date-component ordering,
 * timezone offsets, sub-microsecond precision — returns `false` so the
 * caller can fall back to the generic `date::parse` path.
 *
 * Pure: no heap allocation, no iostream, no locale facets. ~50 ns / call
 * after warm-up; designed to remove `date::parse` from the streaming
 * Stage B hot path on the [stream_to_table] benchmark.
 *
 * To match the legacy `TryParseTimestampOnce` failure contract, an epoch-
 * zero result (`time_since_epoch().count() <= 0`) is reported as a parse
 * failure even when the string was structurally valid.
 *
 * @param sv           Timestamp bytes (no surrounding quotes; whitespace is
 *                     not stripped by this function).
 * @param dateTimeSep  `'T'` for `%FT%T`-style inputs, `' '` for `%F %T`.
 * @param out          Output timestamp, written only on success.
 * @return `true` iff @p sv parsed cleanly under the recognised shape.
 */
bool TryParseIsoTimestamp(std::string_view sv, char dateTimeSep, TimeStamp &out);

/**
 * @brief Slow-path equivalent of `TryParseIsoTimestamp` for arbitrary
 *        `date::parse`-compatible @p format strings.
 *
 * Reuses the caller-supplied scratch buffers so the per-call heap traffic
 * is bounded by the size of the input string (one short copy into
 * `scratch.str`); the `std::istringstream` itself is reset via `clear()` +
 * `str()` rather than reconstructed.
 */
bool TryParseGenericTimestamp(
    std::string_view sv, const std::string &format, TimestampParseScratch &scratch, TimeStamp &out
);

/**
 * @brief Dispatcher that picks the fast or slow path based on @p kind and
 *        threads the caller's scratch buffers through to the slow path.
 *
 * Used by both the legacy `ParseTimestampLine` (with thread-local scratch)
 * and the streaming Stage B inline timestamp-promotion loop (with
 * `WorkerState`-owned scratch).
 */
bool TryParseTimestamp(
    std::string_view sv,
    const std::string &format,
    TimestampFormatKind kind,
    TimestampParseScratch &scratch,
    TimeStamp &out
);

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
