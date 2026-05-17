#include "loglib/log_level.hpp"

#include "loglib/internal/ascii_case.hpp"

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace loglib
{

namespace
{

struct LevelAlias
{
    std::string_view alias;
    LogLevel level;
};

/// Built-in alias table. Aliases are compared case-insensitively against
/// the input byte sequence; ordering is informational only. Aliases that
/// map to `LogLevel::Unknown` (the sentinel) are intentionally absent --
/// `ParseLevelName` already treats "no match" as `std::nullopt`, so
/// including such an entry would be dead code.
constexpr std::array<LevelAlias, 21> BUILTIN_ALIASES = {{
    {"trace", LogLevel::Trace},      {"trc", LogLevel::Trace},          {"debug", LogLevel::Debug},
    {"dbg", LogLevel::Debug},        {"verbose", LogLevel::Debug},      {"info", LogLevel::Info},
    {"information", LogLevel::Info}, {"informational", LogLevel::Info}, {"notice", LogLevel::Info},
    {"warn", LogLevel::Warn},        {"warning", LogLevel::Warn},       {"error", LogLevel::Error},
    {"err", LogLevel::Error},        {"severe", LogLevel::Error},       {"fatal", LogLevel::Fatal},
    {"critical", LogLevel::Fatal},   {"crit", LogLevel::Fatal},         {"emerg", LogLevel::Fatal},
    {"emergency", LogLevel::Fatal},  {"panic", LogLevel::Fatal},        {"alert", LogLevel::Fatal},
}};

} // namespace

std::string_view CanonicalLevelName(LogLevel level) noexcept
{
    switch (level)
    {
    case LogLevel::Trace:
        return "Trace";
    case LogLevel::Debug:
        return "Debug";
    case LogLevel::Info:
        return "Info";
    case LogLevel::Warn:
        return "Warn";
    case LogLevel::Error:
        return "Error";
    case LogLevel::Fatal:
        return "Fatal";
    case LogLevel::Unknown:
    default:
        return "Unknown";
    }
}

std::optional<LogLevel> ParseLevelName(std::string_view bytes) noexcept
{
    if (bytes.empty())
    {
        return std::nullopt;
    }
    for (const LevelAlias &entry : BUILTIN_ALIASES)
    {
        if (internal::EqualsIgnoreCaseAscii(bytes, entry.alias))
        {
            return entry.level;
        }
    }
    return std::nullopt;
}

std::optional<LogLevel> ResolveLevel(
    std::string_view bytes, std::span<const std::pair<std::string, std::string>> overrides
) noexcept
{
    // Check overrides first so callers can re-map any alias (including a
    // built-in one) without removing the built-in table.
    for (const auto &[alias, canonicalName] : overrides)
    {
        if (!internal::EqualsIgnoreCaseAscii(bytes, alias))
        {
            continue;
        }
        // Resolve the canonical side via the built-in alias table
        // (so both `"Info"` and `"info"` work). Invalid canonical
        // strings (typos, `"Unknown"`) silently fall through to the
        // next override and ultimately to the built-in lookup below,
        // matching the contract documented in the header.
        if (const auto resolved = ParseLevelName(canonicalName); resolved.has_value())
        {
            return resolved;
        }
    }
    return ParseLevelName(bytes);
}

} // namespace loglib
