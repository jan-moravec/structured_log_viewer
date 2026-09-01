#include "loglib/time_zone_context.hpp"

#include <date/date.h>
#include <date/tz.h>
#include <fmt/format.h>

#include <chrono>
#include <memory>
#include <mutex>
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
        throw std::runtime_error("tzdata directory does not exist: " + tzdataPath.string());
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
            fmt::format("failed to resolve the system time zone from '{}': {}", tzdataPath.string(), ex.what())
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
            fmt::format("timezone '{}' is unavailable in '{}': {}", zoneName, tzdataPath.string(), ex.what())
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
    const std::string formatString{format};
    const auto rounded = std::chrono::round<std::chrono::milliseconds>(timeStamp);
    if (mImpl->zone == nullptr)
    {
        return date::format(formatString, rounded);
    }
    const date::zoned_time localTime{mImpl->zone, rounded};
    return date::format(formatString, localTime);
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

} // namespace loglib
