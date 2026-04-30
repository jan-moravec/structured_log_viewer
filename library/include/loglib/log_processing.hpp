#pragma once

#include "key_index.hpp"
#include "log_configuration.hpp"
#include "log_data.hpp"
#include "log_line.hpp"
#include "stream_log_line.hpp"

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
    KeyId keyId = kInvalidKeyId;
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

/// Promotes one configured `Type::time` column over @p lines in place.
/// Caller must ensure `column.type == Type::time`. Pass a sub-span to
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

/// `StreamLogLine` overload of `BackfillTimestampColumn`. Mirrors the
/// `LogLine` path: walks @p lines, looks up each configured key id, tries
/// the column's `parseFormats` against the value's string bytes (which on
/// `StreamLogLine` are owned `std::string`s — no mmap arena indirection),
/// and on success swaps the value for the parsed `TimeStamp` via
/// `StreamLogLine::SetValue`. Used by `LogTable::AppendBatch` when a new
/// `Type::time` column is observed mid-stream and the existing rows must
/// be back-filled (PRD 4.6.2). Errors are dropped on the streaming hot path.
void BackfillTimestampColumn(
    const LogConfiguration::Column &column, std::span<StreamLogLine> lines, BackfillErrors discardErrors
);

int64_t TimeStampToLocalMillisecondsSinceEpoch(TimeStamp timeStamp);

int64_t UtcMicrosecondsToLocalMilliseconds(int64_t microseconds);

TimeStamp LocalMillisecondsSinceEpochToTimeStamp(int64_t milliseconds);

/// Formats UTC microseconds since epoch as a `%F %T`-style local-time string.
std::string UtcMicrosecondsToDateTimeString(int64_t microseconds);

/// Formats a `TimeStamp` as a `%F %T`-style local-time string.
std::string TimeStampToDateTimeString(TimeStamp timeStamp);

} // namespace loglib
