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

/// Coarse classification of a timestamp `parseFormats` string. `Generic`
/// always falls through to `date::parse`; the other kinds dispatch to a
/// hand-rolled fast path. Adding a new kind is strictly additive — unknown
/// formats keep the legacy semantics.
enum class TimestampFormatKind : std::uint8_t
{
    Generic,
    Iso8601_T,
    Iso8601_Space,
};

/// Returns the fast-path kind for @p format, or `Generic` for any format
/// string that does not match one of the recognised ISO-8601 shapes verbatim
/// (`"%FT%T"` and `"%F %T"`). Pure; cheap enough to call once per column at
/// configuration time.
TimestampFormatKind ClassifyTimestampFormat(std::string_view format);

/// Per-line carry-over for the "remember the last successful (keyId, format)
/// pair" fast path. Reset by the caller at the start of a column walk;
/// callers thread one of these through the per-line helpers and the function
/// updates it on each successful parse. `kind` caches
/// `ClassifyTimestampFormat(format)` so the hot path dispatches on a byte
/// compare rather than re-classifying.
struct LastValidTimestampParse
{
    KeyId keyId = kInvalidKeyId;
    std::string format;
    TimestampFormatKind kind = TimestampFormatKind::Generic;
};

/// Per-caller scratch buffers for the generic `date::parse` fallback. Holds
/// the input-string staging buffer and the `istringstream` used by
/// `date::parse`, both reused across calls so the slow path's per-call heap
/// traffic is bounded by the input size.
struct TimestampParseScratch
{
    std::string str;
    std::istringstream stream;
};

/// Parses an ISO-8601-style timestamp directly from @p sv into @p out.
/// Recognised shape: `YYYY-MM-DD<sep>HH:MM:SS[.fff[fff]]` where `<sep>` is
/// either `'T'` or `' '` (caller picks via @p dateTimeSep). Up to six
/// fractional-second digits are accepted (microsecond precision). Anything
/// else returns `false` so the caller falls back to `date::parse`. To match
/// the legacy `TryParseTimestampOnce` failure contract, an epoch-zero result
/// is reported as a failure even when the string was structurally valid.
///
/// @param sv          Timestamp bytes (no surrounding quotes; whitespace is
///                    not stripped).
/// @param dateTimeSep `'T'` for `%FT%T`-style inputs, `' '` for `%F %T`.
/// @param out         Output timestamp; written only on success.
bool TryParseIsoTimestamp(std::string_view sv, char dateTimeSep, TimeStamp &out);

/// Slow-path equivalent of `TryParseIsoTimestamp` for arbitrary
/// `date::parse`-compatible @p format strings. Reuses @p scratch's buffers;
/// the `istringstream` is reset via `clear()` + `str()` rather than
/// reconstructed.
bool TryParseGenericTimestamp(
    std::string_view sv, const std::string &format, TimestampParseScratch &scratch, TimeStamp &out
);

/// Dispatcher that picks the fast or slow path based on @p kind and threads
/// the caller's scratch buffers through to the slow path.
bool TryParseTimestamp(
    std::string_view sv,
    const std::string &format,
    TimestampFormatKind kind,
    TimestampParseScratch &scratch,
    TimeStamp &out
);

/// Initializes the log processing library with timezone data. Must be called
/// before any other log processing function.
/// @param tzdata Path to the timezone database directory.
void Initialize(const std::filesystem::path &tzdata);

/// Returns the process-wide cached pointer to the current IANA time zone.
/// Must not be called before `Initialize` has installed the tzdata database.
/// The returned pointer is non-owning and never null after successful init.
const date::time_zone *CurrentZone();

/// Parses timestamp columns in @p logData according to @p configuration,
/// replacing matched string values in place with `TimeStamp`s.
/// @return Per-line failure messages, one per line whose configured timestamp
///         column could not be parsed. Empty on full success.
std::vector<std::string> ParseTimestamps(LogData &logData, const LogConfiguration &configuration);

/// Promotes the timestamps of one configured `Type::time` column over a slice
/// of `LogLine`s. Used both by `ParseTimestamps` (whole-data path) and by
/// `LogTable::AppendBatch` as the streaming back-fill safety net for keys
/// that appear after the harness's configuration snapshot.
///
/// @param column Configured `Type::time` column. Caller must ensure
///               `column.type == LogConfiguration::Type::time`; this is not
///               re-checked.
/// @param lines  Mutable slice of lines to back-fill in place.
/// @return Per-line failure messages, one per line that did not match any
///         (key, format) pair. Empty on full success.
std::vector<std::string> BackfillTimestampColumn(const LogConfiguration::Column &column, std::vector<LogLine> &lines);

/// Converts a `TimeStamp` to local-time milliseconds since epoch.
int64_t TimeStampToLocalMillisecondsSinceEpoch(TimeStamp timeStamp);

/// Converts UTC microseconds since epoch to local-time milliseconds since
/// epoch.
int64_t UtcMicrosecondsToLocalMilliseconds(int64_t microseconds);

/// Converts local-time milliseconds since epoch to a `TimeStamp`.
TimeStamp LocalMillisecondsSinceEpochToTimeStamp(int64_t milliseconds);

/// Formats UTC microseconds since epoch as a `%F %T`-style local-time string.
std::string UtcMicrosecondsToDateTimeString(int64_t microseconds);

/// Formats a `TimeStamp` as a `%F %T`-style local-time string.
std::string TimeStampToDateTimeString(TimeStamp timeStamp);

} // namespace loglib
