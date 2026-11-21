/**
 * @brief Time zone rules for converting between UTC and local wall-clock time.
 *
 * A TimeZoneContext loads a zone from the tzdata files and converts timestamps both ways, including the ambiguous and
 * missing hours around daylight saving changes. Callers pass a context explicitly to every function that needs a zone.
 * An application that shows one zone keeps one context and hands it to the objects that need it.
 *
 * This is part of the time module.
 */
#pragma once

#include "loglib/core/log_value.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
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

/** @brief How many digits of the second a formatted time shows: three for milliseconds, six for microseconds. */
enum class FractionPrecision : std::uint8_t
{
    Milliseconds,
    Microseconds,
};

/**
 * @brief A range of UTC instants over which a zone keeps the same offset and abbreviation, for example one half of a
 * year between two daylight saving changes.
 */
struct ZoneSpan
{
    /** @brief The first UTC microsecond in the span. */
    std::int64_t beginUtc = 0;
    /** @brief One past the last UTC microsecond in the span. */
    std::int64_t endUtc = 0;
    /** @brief The seconds to add to a UTC time to get the local time. */
    std::int64_t offsetSeconds = 0;
    /** @brief The short zone name, such as CET. */
    std::string abbreviation;
};

/**
 * @brief A time zone that converts between local time and UTC and formats times in that zone.
 *
 * A context holds an IANA zone and cannot be changed after it is built. Building it is the only step that can fail,
 * and there is no separate initialization. Copies share the same data and can be used by several threads at once for
 * converting and formatting.
 *
 * Loading time zone data is process-wide: the first Load installs a tzdata folder for the whole process, and a later
 * Load from a different folder throws FormatError. Loading again from the same folder, for another zone, is fine. The
 * selected zone belongs to each context. A default-constructed context is UTC and needs no time zone data.
 */
class TimeZoneContext
{
public:
    /** @brief Creates a UTC context, which needs no time zone data. */
    TimeZoneContext();

    /**
     * @brief Loads time zone data from a folder and selects the zone the computer is set to.
     *
     * The folder must exist and hold IANA time zone data. Throws IoError if the folder does not exist and FormatError
     * if the zone cannot be found or if a different folder was already used by an earlier Load.
     */
    [[nodiscard]] static TimeZoneContext Load(const std::filesystem::path &tzdataPath);

    /**
     * @brief Loads time zone data from a folder and selects a zone by its IANA name, such as Europe/Berlin.
     *
     * Throws IoError if the folder does not exist and FormatError if the zone is not available or if a different folder
     * was already used by an earlier Load.
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
    [[nodiscard]] std::int64_t LocalToUtc(std::int64_t localMicroseconds) const;

    /**
     * @brief Returns the range of local times around a time that share its UTC offset.
     *
     * Inside the window, subtracting utcOffset gives the same result as LocalToUtc without a zone lookup, which makes
     * converting many nearby times cheap. The window stays away from clock changes, so it is empty when the time is
     * close to one.
     */
    [[nodiscard]] LocalOffsetWindow OffsetWindowAt(std::int64_t localMicroseconds) const;

    /** @brief Converts UTC microseconds since the epoch to local microseconds, the wall-clock reading in this zone. */
    [[nodiscard]] std::int64_t UtcToLocal(std::int64_t utcMicroseconds) const;

    /** @brief Formats a time in this zone with a strftime-style format, rounded to milliseconds. */
    [[nodiscard]] std::string Format(TimeStamp timeStamp, std::string_view format) const;

    /**
     * @brief Appends the text that Format gives to out. The text is written by a formatter that each thread keeps for
     * its last zone and format, so a loop over the cells of one column allocates nothing. Throws std::runtime_error if
     * the format cannot be applied, and then out is unchanged.
     */
    void AppendFormatted(std::string &out, TimeStamp timeStamp, std::string_view format) const;

    /**
     * @brief Formats a time in this zone with a strftime-style format, with the given number of fraction digits.
     *
     * Milliseconds round the time to the nearest millisecond first. A time within three days of either end of the
     * TimeStamp range is moved inward to that limit, so the arithmetic cannot overflow. This always uses the general
     * formatter, so it is the reference for faster ways of writing the same text. Throws std::runtime_error if the
     * format cannot be applied.
     */
    [[nodiscard]] std::string Format(TimeStamp timeStamp, std::string_view format, FractionPrecision precision) const;

    /** @brief True for a context that has no zone data, which is UTC. */
    [[nodiscard]] bool IsUtc() const noexcept;

    /**
     * @brief Returns the span of UTC instants around a time that share its offset and abbreviation.
     *
     * The result is empty if the zone data cannot answer for that time. A UTC context returns one span for all time.
     */
    [[nodiscard]] std::optional<ZoneSpan> ZoneSpanAt(std::int64_t utcMicroseconds) const;

    /** @brief Formats a time as local time in this zone, as year-month-day hours:minutes:seconds. */
    [[nodiscard]] std::string FormatDateTime(TimeStamp timeStamp) const;

private:
    struct Impl;

    explicit TimeZoneContext(std::shared_ptr<const Impl> impl);

    std::shared_ptr<const Impl> mImpl;
};

} // namespace loglib
