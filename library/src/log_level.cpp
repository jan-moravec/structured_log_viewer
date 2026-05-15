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
/// the input byte sequence; ordering is informational only.
constexpr std::array<LevelAlias, 22> BUILTIN_ALIASES = {{
    {"trace", LogLevel::Trace},      {"trc", LogLevel::Trace},          {"debug", LogLevel::Debug},
    {"dbg", LogLevel::Debug},        {"verbose", LogLevel::Debug},      {"info", LogLevel::Info},
    {"information", LogLevel::Info}, {"informational", LogLevel::Info}, {"notice", LogLevel::Info},
    {"warn", LogLevel::Warn},        {"warning", LogLevel::Warn},       {"error", LogLevel::Error},
    {"err", LogLevel::Error},        {"severe", LogLevel::Error},       {"fatal", LogLevel::Fatal},
    {"critical", LogLevel::Fatal},   {"crit", LogLevel::Fatal},         {"emerg", LogLevel::Fatal},
    {"emergency", LogLevel::Fatal},  {"panic", LogLevel::Fatal},        {"alert", LogLevel::Fatal},
    {"unknown", LogLevel::Unknown},
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
            // The `"unknown"` alias maps to `LogLevel::Unknown`, which we
            // treat as "did not match a real level" for promotion-decision
            // purposes. Surface it via the same `nullopt` so callers don't
            // need to branch on the sentinel.
            if (entry.level == LogLevel::Unknown)
            {
                return std::nullopt;
            }
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
        // Resolve the canonical side by name. Invalid canonical strings
        // are silently ignored so a typo in saved config can't crash the
        // resolver.
        if (const auto resolved = ParseLevelName(canonicalName); resolved.has_value())
        {
            return resolved;
        }
        // Also accept the canonical form (`"Info"`) which `ParseLevelName`
        // happens to recognise via the alias `"info"`; the lookup above
        // already handled it. Fall through to try the next override (in
        // case the user listed multiple entries for the same alias).
    }
    return ParseLevelName(bytes);
}

} // namespace loglib
