#pragma once

#include <loglib/log_level.hpp>

#include <string_view>

namespace slp
{

/** @brief ANSI sequence that ends any styling started by `LevelAnsiPrefix`. */
inline constexpr std::string_view ANSI_RESET = "\033[0m";

/** @brief ANSI sequence that starts the color for @p level, or empty for `Unknown`. */
[[nodiscard]] constexpr std::string_view LevelAnsiPrefix(loglib::LogLevel level) noexcept
{
    switch (level)
    {
    case loglib::LogLevel::Trace:
        return "\033[2m";
    case loglib::LogLevel::Debug:
        return "\033[36m";
    case loglib::LogLevel::Info:
        return "\033[32m";
    case loglib::LogLevel::Warn:
        return "\033[33m";
    case loglib::LogLevel::Error:
        return "\033[31m";
    case loglib::LogLevel::Fatal:
        return "\033[1;35m";
    case loglib::LogLevel::Unknown:
        return {};
    }
    return {};
}

} // namespace slp
