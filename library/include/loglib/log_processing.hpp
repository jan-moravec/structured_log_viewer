#pragma once

#include "key_index.hpp"
#include "log_configuration.hpp"
#include "log_data.hpp"
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

/// Classification of a `parseFormats` string. `Generic` falls through to
/// `date::parse`; the others dispatch to a hand-rolled fast path.
enum class TimestampFormatKind : std::uint8_t
{
    Generic,
    Iso8601_T,
    Iso8601_Space,
};

/// Returns the fast-path kind for @p format (`"%FT%T"` / `"%F %T"`), else `Generic`.
TimestampFormatKind ClassifyTimestampFormat(std::string_view format);

/// Per-line carry-over for the "remember the last successful (keyId, format)"
/// fast path. `kind` caches `ClassifyTimestampFormat(format)`.
struct LastValidTimestampParse
{
    KeyId keyId = INVALID_KEY_ID;
    std::string format;
    TimestampFormatKind kind = TimestampFormatKind::Generic;
};

/// Reusable scratch for the generic `date::parse` fallback.
struct TimestampParseScratch
{
    std::string str;
    std::istringstream stream;
};

/// ISO-8601 fast path. Accepts `YYYY-MM-DD<sep>HH:MM:SS[.fff[fff]]` with up to
/// six fractional digits; @p dateTimeSep is `'T'` or `' '`. An epoch-zero
/// result is reported as a failure (legacy contract).
bool TryParseIsoTimestamp(std::string_view sv, char dateTimeSep, TimeStamp &out);

/// Slow-path `date::parse` fallback; reuses @p scratch across calls.
bool TryParseGenericTimestamp(
    std::string_view sv, const std::string &format, TimestampParseScratch &scratch, TimeStamp &out
);

/// Picks the fast or slow path based on @p kind.
bool TryParseTimestamp(
    std::string_view sv,
    const std::string &format,
    TimestampFormatKind kind,
    TimestampParseScratch &scratch,
    TimeStamp &out
);

/// Installs the timezone database. Must be called before any other timestamp
/// helper in this header.
void Initialize(const std::filesystem::path &tzdata);

/// Process-wide cached current IANA zone. Non-null after successful `Initialize`.
const date::time_zone *CurrentZone();

/// Promotes timestamp columns in @p logData; returns per-line failure messages.
std::vector<std::string> ParseTimestamps(LogData &logData, const LogConfiguration &configuration);

/// Promotes one configured `Type::Time` column over @p lines in place.
/// Caller must ensure `column.type == Type::Time`. Pass a sub-span to
/// restrict the back-fill to a slice of a larger vector (e.g. only the rows
/// just appended in a streaming batch). Returns per-line failure messages.
std::vector<std::string> BackfillTimestampColumn(const LogConfiguration::Column &column, std::span<LogLine> lines);

/// Tag selecting the `void` overload that skips per-line "Failed to parse"
/// formatting on the streaming hot path.
enum class BackfillErrors : uint8_t
{
    Discard
};

/// `void` overload of `BackfillTimestampColumn` that drops error messages.
void BackfillTimestampColumn(
    const LogConfiguration::Column &column, std::span<LogLine> lines, BackfillErrors discardErrors
);

int64_t TimeStampToLocalMillisecondsSinceEpoch(TimeStamp timeStamp);

int64_t UtcMicrosecondsToLocalMilliseconds(int64_t microseconds);

TimeStamp LocalMillisecondsSinceEpochToTimeStamp(int64_t milliseconds);

/// Convert @p localMicroseconds -- interpreted as a wall-clock instant
/// in @p zone -- to UTC microseconds since epoch. Returns the naive
/// value unchanged when @p zone is `nullptr`. Uses
/// `date::to_sys(local, choose::earliest)` so DST edge cases resolve
/// without throwing:
///   * an ambiguous local time (the "fall-back" hour) yields the
///     earlier of the two candidates.
///   * a nonexistent local time (the "spring-forward" gap) snaps to
///     the sys_time transition boundary between the two offsets --
///     i.e., the first real instant *after* the gap. Callers that
///     require a naive-value fallback for gaps would need to probe
///     `zone->get_info(local)` up-front; the Goto Timestamp caller
///     accepts the transition-boundary outcome as the natural
///     "nearest existing instant" target.
///
/// Non-DST exceptions from the underlying zone lookup (e.g. inputs
/// past the tzdata transition table, or a corrupt zone entry) are
/// caught and yield the naive value so the Goto Timestamp slot
/// stays exception-safe.
///
/// The zone parameter exists so tests can pin `America/New_York` /
/// `Europe/Berlin` DST transitions deterministically across CI
/// hosts. Production callers use the no-argument overload below,
/// which resolves to `CurrentZone()`.
int64_t LocalMicrosecondsSinceEpochToUtc(int64_t localMicroseconds, const date::time_zone *zone);

/// Production overload: convert against `CurrentZone()`. Equivalent
/// to `LocalMicrosecondsSinceEpochToUtc(local, CurrentZone())` and
/// preserves the pre-existing call-site shape.
int64_t LocalMicrosecondsSinceEpochToUtc(int64_t localMicroseconds);

/// Formats UTC microseconds since epoch as a `%F %T`-style local-time string.
std::string UtcMicrosecondsToDateTimeString(int64_t microseconds);

/// Formats a `TimeStamp` as a `%F %T`-style local-time string.
std::string TimeStampToDateTimeString(TimeStamp timeStamp);

} // namespace loglib
