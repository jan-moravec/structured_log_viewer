#include "loglib/time/time_zone_context.hpp"
#include "loglib/core/internal/path_encoding.hpp"
#include "loglib/time/internal/timestamp_formatter.hpp"

#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>

#include <chrono>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace loglib
{

struct TimeZoneContext::Impl
{
    const date::time_zone *zone = nullptr;
    std::string ianaName{"UTC"};
};

namespace
{

std::mutex &DefaultContextMutex()
{
    static std::mutex mutex;
    return mutex;
}

TimeZoneContext &DefaultContextSlot()
{
    static TimeZoneContext context;
    return context;
}

void InstallTzdata(const std::filesystem::path &tzdataPath)
{
    std::error_code existsEc;
    if (!std::filesystem::exists(tzdataPath, existsEc) || !std::filesystem::is_directory(tzdataPath))
    {
        throw std::runtime_error("tzdata directory does not exist: " + internal::PathToUtf8(tzdataPath));
    }
    date::set_install(tzdataPath.string());
}

} // namespace

TimeZoneContext::TimeZoneContext()
    : mImpl(std::make_shared<Impl>())
{
}

TimeZoneContext::TimeZoneContext(std::shared_ptr<const Impl> impl)
    : mImpl(std::move(impl))
{
}

TimeZoneContext TimeZoneContext::Utc()
{
    return TimeZoneContext{};
}

TimeZoneContext TimeZoneContext::Load(const std::filesystem::path &tzdataPath)
{
    InstallTzdata(tzdataPath);
    try
    {
        const date::time_zone *zone = date::current_zone();
        if (zone == nullptr)
        {
            throw std::runtime_error("current_zone returned null");
        }
        auto impl = std::make_shared<Impl>();
        impl->zone = zone;
        impl->ianaName = std::string(zone->name());
        return TimeZoneContext{std::move(impl)};
    }
    catch (const std::exception &ex)
    {
        throw std::runtime_error(
            fmt::format(
                "failed to resolve the system time zone from '{}': {}", internal::PathToUtf8(tzdataPath), ex.what()
            )
        );
    }
}

TimeZoneContext TimeZoneContext::Load(const std::filesystem::path &tzdataPath, std::string_view zoneName)
{
    InstallTzdata(tzdataPath);
    try
    {
        const date::time_zone *zone = date::locate_zone(std::string{zoneName});
        if (zone == nullptr)
        {
            throw std::runtime_error("locate_zone returned null");
        }
        auto impl = std::make_shared<Impl>();
        impl->zone = zone;
        impl->ianaName = std::string(zone->name());
        return TimeZoneContext{std::move(impl)};
    }
    catch (const std::exception &ex)
    {
        throw std::runtime_error(
            fmt::format(
                "timezone '{}' is unavailable in '{}': {}", zoneName, internal::PathToUtf8(tzdataPath), ex.what()
            )
        );
    }
}

std::string_view TimeZoneContext::IanaName() const noexcept
{
    return mImpl->ianaName;
}

std::int64_t TimeZoneContext::LocalMicrosecondsToUtc(std::int64_t localMicroseconds) const
{
    if (mImpl->zone == nullptr)
    {
        return localMicroseconds;
    }
    const date::local_time<std::chrono::microseconds> localTime{std::chrono::microseconds{localMicroseconds}};
    try
    {
        const auto systemTime = mImpl->zone->to_sys(localTime, date::choose::earliest);
        return systemTime.time_since_epoch().count();
    }
    catch (const std::exception &)
    {
        return localMicroseconds;
    }
}

LocalOffsetWindow TimeZoneContext::OffsetWindowAt(std::int64_t localMicroseconds) const
{
    if (mImpl->zone == nullptr)
    {
        return {
            .beginLocal = std::numeric_limits<std::int64_t>::min(),
            .endLocal = std::numeric_limits<std::int64_t>::max(),
            .utcOffset = 0,
        };
    }
    const date::local_time<std::chrono::microseconds> localTime{std::chrono::microseconds{localMicroseconds}};
    try
    {
        const auto info = mImpl->zone->get_info(localTime);
        if (info.result != date::local_info::unique)
        {
            return {};
        }
        // Stay a day clear of transitions, where a wall clock can repeat or not exist.
        constexpr std::chrono::microseconds MARGIN = std::chrono::hours{24};
        const std::chrono::microseconds offset = info.first.offset;
        const std::chrono::microseconds begin = info.first.begin.time_since_epoch();
        const std::chrono::microseconds end = info.first.end.time_since_epoch();
        return {
            .beginLocal = (begin + offset + MARGIN).count(),
            .endLocal = (end + offset - MARGIN).count(),
            .utcOffset = offset.count(),
        };
    }
    catch (const std::exception &)
    {
        return {};
    }
}

std::int64_t TimeZoneContext::ToLocalMilliseconds(TimeStamp timeStamp) const
{
    if (mImpl->zone == nullptr)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(timeStamp.time_since_epoch()).count();
    }
    const auto zonedTime = date::zoned_time{mImpl->zone, timeStamp};
    const auto localTime = zonedTime.get_local_time();
    return std::chrono::duration_cast<std::chrono::milliseconds>(localTime.time_since_epoch()).count();
}

std::int64_t TimeZoneContext::UtcMicrosecondsToLocalMilliseconds(std::int64_t microseconds) const
{
    const std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds> utcTime{
        std::chrono::microseconds{microseconds}
    };
    if (mImpl->zone == nullptr)
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(utcTime.time_since_epoch()).count();
    }
    const date::zoned_time localTime{mImpl->zone, utcTime};
    return std::chrono::duration_cast<std::chrono::milliseconds>(localTime.get_local_time().time_since_epoch()).count();
}

TimeStamp TimeZoneContext::LocalMillisecondsToUtc(std::int64_t milliseconds) const
{
    const std::int64_t localMicroseconds =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::milliseconds{milliseconds}).count();
    return TimeStamp{std::chrono::microseconds{LocalMicrosecondsToUtc(localMicroseconds)}};
}

std::string TimeZoneContext::Format(TimeStamp timeStamp, std::string_view format) const
{
    std::string text;
    AppendFormatted(text, timeStamp, format);
    return text;
}

void TimeZoneContext::AppendFormatted(std::string &out, TimeStamp timeStamp, std::string_view format) const
{
    // Each thread keeps the formatter of its last zone and format, because callers format one column's cells in a row.
    struct Cached
    {
        std::shared_ptr<const Impl> impl;
        std::string format;
        std::optional<internal::TimestampFormatter> formatter;
    };
    thread_local Cached cached;
    if (!cached.formatter || cached.impl != mImpl || cached.format != format)
    {
        cached.formatter.emplace(*this, format, FractionPrecision::Milliseconds);
        cached.impl = mImpl;
        cached.format = format;
    }
    cached.formatter->Append(out, timeStamp);
}

std::string TimeZoneContext::Format(TimeStamp timeStamp, std::string_view format, FractionPrecision precision) const
{
    const std::string formatString{format};
    const auto formatWith = [&]<typename Duration>(Duration) {
        const auto rounded = std::chrono::round<Duration>(timeStamp);
        if (mImpl->zone == nullptr)
        {
            return date::format(formatString, rounded);
        }
        const date::zoned_time localTime{mImpl->zone, rounded};
        return date::format(formatString, localTime);
    };
    if (precision == FractionPrecision::Milliseconds)
    {
        return formatWith(std::chrono::milliseconds{});
    }
    return formatWith(std::chrono::microseconds{});
}

bool TimeZoneContext::IsUtc() const noexcept
{
    return mImpl->zone == nullptr;
}

std::optional<ZoneSpan> TimeZoneContext::ZoneSpanAt(std::int64_t utcMicroseconds) const
{
    if (mImpl->zone == nullptr)
    {
        return ZoneSpan{
            .beginUtc = std::numeric_limits<std::int64_t>::min(),
            .endUtc = std::numeric_limits<std::int64_t>::max(),
            .offsetSeconds = 0,
            .abbreviation = "UTC",
        };
    }
    // Instants that the zone reports as unbounded saturate instead of overflowing.
    constexpr std::int64_t SECONDS_LIMIT = std::numeric_limits<std::int64_t>::max() / 1'000'000;
    const auto toMicroseconds = [](std::chrono::seconds seconds) {
        if (seconds.count() >= SECONDS_LIMIT)
        {
            return std::numeric_limits<std::int64_t>::max();
        }
        if (seconds.count() <= -SECONDS_LIMIT)
        {
            return std::numeric_limits<std::int64_t>::min();
        }
        return seconds.count() * 1'000'000;
    };
    try
    {
        const date::sys_time<std::chrono::microseconds> instant{std::chrono::microseconds{utcMicroseconds}};
        const date::sys_info info = mImpl->zone->get_info(instant);
        return ZoneSpan{
            .beginUtc = toMicroseconds(info.begin.time_since_epoch()),
            .endUtc = toMicroseconds(info.end.time_since_epoch()),
            .offsetSeconds = info.offset.count(),
            .abbreviation = info.abbrev,
        };
    }
    catch (const std::exception &)
    {
        return std::nullopt;
    }
}

std::string TimeZoneContext::FormatUtcMicroseconds(std::int64_t microseconds) const
{
    const TimeStamp utcTime{std::chrono::microseconds{microseconds}};
    return Format(utcTime, "%F %T");
}

std::string TimeZoneContext::FormatTimeStamp(TimeStamp timeStamp) const
{
    return Format(timeStamp, "%F %T");
}

void SetProcessDefaultTimeZone(TimeZoneContext context)
{
    const std::scoped_lock lock(DefaultContextMutex());
    DefaultContextSlot() = std::move(context);
}

TimeZoneContext ProcessDefaultTimeZone()
{
    const std::scoped_lock lock(DefaultContextMutex());
    return DefaultContextSlot();
}

int64_t UtcMicrosecondsToLocalMilliseconds(int64_t microseconds, const TimeZoneContext &timeZone)
{
    return timeZone.UtcMicrosecondsToLocalMilliseconds(microseconds);
}

int64_t UtcMicrosecondsToLocalMilliseconds(int64_t microseconds)
{
    return UtcMicrosecondsToLocalMilliseconds(microseconds, ProcessDefaultTimeZone());
}

TimeStamp LocalMillisecondsSinceEpochToTimeStamp(int64_t milliseconds, const TimeZoneContext &timeZone)
{
    return timeZone.LocalMillisecondsToUtc(milliseconds);
}

TimeStamp LocalMillisecondsSinceEpochToTimeStamp(int64_t milliseconds)
{
    return LocalMillisecondsSinceEpochToTimeStamp(milliseconds, ProcessDefaultTimeZone());
}

int64_t LocalMicrosecondsSinceEpochToUtc(int64_t localMicroseconds, const TimeZoneContext &timeZone)
{
    return timeZone.LocalMicrosecondsToUtc(localMicroseconds);
}

int64_t LocalMicrosecondsSinceEpochToUtc(int64_t localMicroseconds)
{
    return LocalMicrosecondsSinceEpochToUtc(localMicroseconds, ProcessDefaultTimeZone());
}

std::string UtcMicrosecondsToDateTimeString(int64_t microseconds, const TimeZoneContext &timeZone)
{
    return timeZone.FormatUtcMicroseconds(microseconds);
}

std::string UtcMicrosecondsToDateTimeString(int64_t microseconds)
{
    return UtcMicrosecondsToDateTimeString(microseconds, ProcessDefaultTimeZone());
}

} // namespace loglib
