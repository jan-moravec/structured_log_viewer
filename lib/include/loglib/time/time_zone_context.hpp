/**
 * @brief Time zone rules for converting between UTC and local wall-clock time.
 *
 * A TimeZoneContext loads a zone from the tzdata files and converts timestamps both ways, including the ambiguous and
 * missing hours around daylight saving changes. Callers pass a context explicitly. A process-wide default exists for
 * formatting helpers that have no context at hand. The header also holds the small conversion helpers built on it.
 *
 * This is part of the time module.
 */
#pragma once

#include "loglib/core/log_value.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace loglib
{

/**
 * @brief A range of local times that all have the same offset from UTC. A default-constructed window contains nothing.
 */
struct LocalOffsetWindow
{
    /** @brief The first local microsecond in the window. */
    std::int64_t beginLocal = 0;
    /** @brief One past the last local microsecond in the window. */
    std::int64_t endLocal = 0;
    /** @brief The microseconds to subtract from a local time to get UTC. */
    std::int64_t utcOffset = 0;

    /** @brief True if the local time is inside the window. */
    [[nodiscard]] bool Contains(std::int64_t localMicroseconds) const noexcept
    {
        return beginLocal <= localMicroseconds && localMicroseconds < endLocal;
    }
};

/**
 * @brief A time zone that converts between local time and UTC and formats times in that zone.
 *
 * A context holds an IANA zone and cannot be changed after it is built. Building it is the only step that can fail,
 * and there is no separate initialization. Copies share the same data and can be used by several threads at once for
 * converting and formatting.
 *
 * Loading time zone data is process-wide, so loading from different data paths in one process is not supported. The
 * selected zone belongs to each context. A default-constructed context is UTC and needs no time zone data.
 *
 * Conversion functions take a context as an argument. SetProcessDefaultTimeZone also sets one process-wide context for
 * the formatting helpers that do not take one, such as TimeStampToDateTimeString.
 */
class TimeZoneContext
{
public:
    /** @brief Creates a UTC context, which needs no time zone data. */
    TimeZoneContext();

    /**
     * @brief Loads time zone data from a folder and selects the zone the computer is set to.
     *
     * The folder must exist and hold IANA time zone data. Throws std::runtime_error if the path is invalid or the zone
     * cannot be found.
     */
    [[nodiscard]] static TimeZoneContext Load(const std::filesystem::path &tzdataPath);

    /**
     * @brief Loads time zone data from a folder and selects a zone by its IANA name, such as Europe/Berlin.
     *
     * Throws std::runtime_error if the path is invalid or the zone is not available.
     */
    [[nodiscard]] static TimeZoneContext Load(const std::filesystem::path &tzdataPath, std::string_view zoneName);

    /** @brief Creates a UTC context, which needs no time zone data. */
    [[nodiscard]] static TimeZoneContext Utc();

    TimeZoneContext(const TimeZoneContext &) = default;
    TimeZoneContext &operator=(const TimeZoneContext &) = default;
    TimeZoneContext(TimeZoneContext &&) noexcept = default;
    TimeZoneContext &operator=(TimeZoneContext &&) noexcept = default;
    ~TimeZoneContext() = default;

    /** @brief The IANA name of the zone, or UTC for a default-constructed context. */
    [[nodiscard]] std::string_view IanaName() const noexcept;

    /**
     * @brief Converts local microseconds to UTC microseconds.
     *
     * A local time that occurs twice when the clocks go back gives the earlier instant. A local time that does not
     * occur when the clocks go forward gives the moment of the change. If the conversion fails, because the date is too
     * far in the future or the zone data is damaged, the value is returned unchanged.
     */
    [[nodiscard]] std::int64_t LocalMicrosecondsToUtc(std::int64_t localMicroseconds) const;

    /**
     * @brief Returns the range of local times around a time that share its UTC offset.
     *
     * Inside the window, subtracting utcOffset gives the same result as LocalMicrosecondsToUtc without a zone lookup,
     * which makes converting many nearby times cheap. The window stays away from clock changes, so it is empty when the
     * time is close to one.
     */
    [[nodiscard]] LocalOffsetWindow OffsetWindowAt(std::int64_t localMicroseconds) const;

    /** @brief Converts a UTC time to local milliseconds since the epoch. */
    [[nodiscard]] std::int64_t ToLocalMilliseconds(TimeStamp timeStamp) const;

    /** @brief Converts UTC microseconds since the epoch to local milliseconds. */
    [[nodiscard]] std::int64_t UtcMicrosecondsToLocalMilliseconds(std::int64_t microseconds) const;

    /**
     * @brief Reads local milliseconds as a time in this zone and returns the UTC time.
     *
     * Times that occur twice or not at all, and failures, are handled as in LocalMicrosecondsToUtc. On failure the
     * milliseconds are taken as UTC.
     */
    [[nodiscard]] TimeStamp LocalMillisecondsToUtc(std::int64_t milliseconds) const;

    /** @brief Formats a time in this zone with a strftime-style format. */
    [[nodiscard]] std::string Format(TimeStamp timeStamp, std::string_view format) const;

    /** @brief Formats UTC microseconds as local time in this zone, as year-month-day hours:minutes:seconds. */
    [[nodiscard]] std::string FormatUtcMicroseconds(std::int64_t microseconds) const;

    /** @brief Formats a time as local time in this zone, as year-month-day hours:minutes:seconds. */
    [[nodiscard]] std::string FormatTimeStamp(TimeStamp timeStamp) const;

private:
    struct Impl;

    explicit TimeZoneContext(std::shared_ptr<const Impl> impl);

    std::shared_ptr<const Impl> mImpl;
};

/**
 * @brief Sets the process-wide context used by the formatting functions that do not take one.
 *
 * It starts as UTC. The application should call it once at startup, after loading the time zone data. Functions that
 * take a context do not depend on it.
 */
void SetProcessDefaultTimeZone(TimeZoneContext context);

/** @brief The context set by SetProcessDefaultTimeZone, or UTC if none was set. */
[[nodiscard]] TimeZoneContext ProcessDefaultTimeZone();

/**
 * @brief Converts UTC microseconds since the epoch to local milliseconds. The overload without a context uses the
 * process-wide one.
 */
int64_t UtcMicrosecondsToLocalMilliseconds(int64_t microseconds, const TimeZoneContext &timeZone);
int64_t UtcMicrosecondsToLocalMilliseconds(int64_t microseconds);

/**
 * @brief Reads local milliseconds since the epoch as a time in a zone and returns the UTC time. The overload without a
 * context uses the process-wide one.
 */
TimeStamp LocalMillisecondsSinceEpochToTimeStamp(int64_t milliseconds, const TimeZoneContext &timeZone);
TimeStamp LocalMillisecondsSinceEpochToTimeStamp(int64_t milliseconds);

/**
 * @brief Reads local microseconds as a time in a zone and returns UTC microseconds since the epoch.
 *
 * A time that occurs twice when the clocks go back gives the earlier instant. A time that does not occur when the
 * clocks go forward gives the first instant after the gap. If the date is too far ahead or the zone data is damaged,
 * the value is returned unchanged, so callers do not need to handle exceptions. A UTC context returns the value as it
 * is.
 */
int64_t LocalMicrosecondsSinceEpochToUtc(int64_t localMicroseconds, const TimeZoneContext &timeZone);

/** @brief Same as above, with the process-wide context. */
int64_t LocalMicrosecondsSinceEpochToUtc(int64_t localMicroseconds);

/**
 * @brief Formats UTC microseconds since the epoch as local time, as year-month-day hours:minutes:seconds. The overload
 * without a context uses the process-wide one.
 */
std::string UtcMicrosecondsToDateTimeString(int64_t microseconds, const TimeZoneContext &timeZone);
std::string UtcMicrosecondsToDateTimeString(int64_t microseconds);

} // namespace loglib
