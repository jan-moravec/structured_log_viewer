#pragma once

#include "log_value.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace loglib
{

/**
 * @brief Immutable timezone used for local/UTC conversion and formatting.
 *
 * Owns the selected IANA zone after validating tzdata. Copies share the
 * implementation and are safe for concurrent read-only conversion and
 * formatting. Construction is the only point that can fail; later calls
 * do not depend on a separate `Initialize` step.
 *
 * Howard Hinnant `date` installs tzdata process-wide. `Load` with
 * conflicting tzdata paths in one process is unsupported. The selected
 * zone is per context.
 *
 * A default-constructed context is UTC and does not require tzdata.
 * `SetProcessDefaultTimeZone` is a documented process-wide convenience
 * for formatting helpers such as `TimeStampToDateTimeString`; core
 * conversion APIs take an explicit `TimeZoneContext`.
 */
class TimeZoneContext
{
public:
    /** @brief UTC identity conversions that do not require tzdata. */
    TimeZoneContext();

    /**
     * @brief Loads tzdata from @p tzdataPath and selects the system zone.
     *
     * @param tzdataPath Directory containing IANA tzdata. Must exist.
     * @return Context bound to `date::current_zone()`.
     * @throws std::runtime_error if the path is invalid or the zone cannot
     *     be resolved.
     */
    [[nodiscard]] static TimeZoneContext Load(const std::filesystem::path &tzdataPath);

    /**
     * @brief Loads tzdata from @p tzdataPath and selects @p zoneName.
     *
     * @param tzdataPath Directory containing IANA tzdata. Must exist.
     * @param zoneName IANA identifier such as `Europe/Berlin`.
     * @return Context bound to the named zone.
     * @throws std::runtime_error if the path is invalid or the zone is
     *     unavailable.
     */
    [[nodiscard]] static TimeZoneContext Load(const std::filesystem::path &tzdataPath, std::string_view zoneName);

    /** @brief UTC context that does not require tzdata. */
    [[nodiscard]] static TimeZoneContext Utc();

    TimeZoneContext(const TimeZoneContext &) = default;
    TimeZoneContext &operator=(const TimeZoneContext &) = default;
    TimeZoneContext(TimeZoneContext &&) noexcept = default;
    TimeZoneContext &operator=(TimeZoneContext &&) noexcept = default;
    ~TimeZoneContext() = default;

    /** @brief IANA name (`UTC` for the default context). */
    [[nodiscard]] std::string_view IanaName() const noexcept;

    /**
     * @brief Converts wall-clock local microseconds to UTC microseconds.
     *
     * Ambiguous fall-back times choose the earlier instant. Nonexistent
     * spring-forward times resolve to the transition boundary. Far-future
     * or corrupt-zone failures return @p localMicroseconds unchanged.
     */
    [[nodiscard]] std::int64_t LocalMicrosecondsToUtc(std::int64_t localMicroseconds) const;

    /** @brief Converts a UTC `TimeStamp` to local milliseconds since epoch. */
    [[nodiscard]] std::int64_t ToLocalMilliseconds(TimeStamp timeStamp) const;

    /** @brief Converts UTC epoch microseconds to local milliseconds. */
    [[nodiscard]] std::int64_t UtcMicrosecondsToLocalMilliseconds(std::int64_t microseconds) const;

    /**
     * @brief Interprets local milliseconds as a wall-clock time in this zone
     * and returns the UTC `TimeStamp`.
     */
    [[nodiscard]] TimeStamp LocalMillisecondsToUtc(std::int64_t milliseconds) const;

    /** @brief Formats @p timeStamp in this zone with `date::format`. */
    [[nodiscard]] std::string Format(TimeStamp timeStamp, std::string_view format) const;

    /** @brief Formats UTC microseconds as `%F %T` local time in this zone. */
    [[nodiscard]] std::string FormatUtcMicroseconds(std::int64_t microseconds) const;

    /** @brief Formats @p timeStamp as `%F %T` local time in this zone. */
    [[nodiscard]] std::string FormatTimeStamp(TimeStamp timeStamp) const;

private:
    struct Impl;

    explicit TimeZoneContext(std::shared_ptr<const Impl> impl);

    std::shared_ptr<const Impl> mImpl;
};

/**
 * @brief Replaces the process-wide convenience context used by formatting
 * overloads that do not take an explicit `TimeZoneContext`.
 *
 * Starts as UTC. The application should call this once at bootstrap after
 * `Load`. It is not a prerequisite for core APIs that take a context.
 */
void SetProcessDefaultTimeZone(TimeZoneContext context);

/** @brief Process-wide convenience context. Never unset; defaults to UTC. */
[[nodiscard]] TimeZoneContext ProcessDefaultTimeZone();

} // namespace loglib
