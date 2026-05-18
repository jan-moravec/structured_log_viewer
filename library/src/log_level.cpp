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

/// Built-in alias table. Compared case-insensitively; ordering is
/// informational only. Aliases for `Unknown` are omitted because
/// `ParseLevelName` already returns `std::nullopt` on no match.
///
/// Notes:
///   - Single-letter aliases (`t`/`d`/`v`/`i`/`w`/`e`/`f`) cover
///     Android, glog, and many embedded loggers. False positives are
///     guarded by `IsLogLevelKey` upstream.
///   - `v`/`vrb`/`verbose` map to `Debug` for backwards compatibility,
///     even though Serilog/Android treat Verbose as below Debug.
///   - Numeric levels (Bunyan/Pino, syslog) are omitted: conventions
///     disagree, and numeric JSON arrives as `Int64` and never reaches
///     the dictionary. Use per-column `levelMapping` instead.
constexpr std::array<LevelAlias, 37> BUILTIN_ALIASES = {{
    // Trace
    {.alias = "trace", .level = LogLevel::Trace},
    {.alias = "trc", .level = LogLevel::Trace},
    {.alias = "t", .level = LogLevel::Trace},
    {.alias = "finer", .level = LogLevel::Trace},
    {.alias = "finest", .level = LogLevel::Trace},
    {.alias = "silly", .level = LogLevel::Trace},
    // Debug
    {.alias = "debug", .level = LogLevel::Debug},
    {.alias = "dbg", .level = LogLevel::Debug},
    {.alias = "d", .level = LogLevel::Debug},
    {.alias = "verbose", .level = LogLevel::Debug},
    {.alias = "vrb", .level = LogLevel::Debug},
    {.alias = "v", .level = LogLevel::Debug},
    {.alias = "fine", .level = LogLevel::Debug},
    // Info
    {.alias = "info", .level = LogLevel::Info},
    {.alias = "inf", .level = LogLevel::Info},
    {.alias = "i", .level = LogLevel::Info},
    {.alias = "information", .level = LogLevel::Info},
    {.alias = "informational", .level = LogLevel::Info},
    {.alias = "notice", .level = LogLevel::Info},
    // Warn
    {.alias = "warn", .level = LogLevel::Warn},
    {.alias = "wrn", .level = LogLevel::Warn},
    {.alias = "w", .level = LogLevel::Warn},
    {.alias = "warning", .level = LogLevel::Warn},
    // Error
    {.alias = "error", .level = LogLevel::Error},
    {.alias = "err", .level = LogLevel::Error},
    {.alias = "e", .level = LogLevel::Error},
    {.alias = "severe", .level = LogLevel::Error},
    // Fatal
    {.alias = "fatal", .level = LogLevel::Fatal},
    {.alias = "ftl", .level = LogLevel::Fatal},
    {.alias = "f", .level = LogLevel::Fatal},
    {.alias = "critical", .level = LogLevel::Fatal},
    {.alias = "crit", .level = LogLevel::Fatal},
    {.alias = "emerg", .level = LogLevel::Fatal},
    {.alias = "emergency", .level = LogLevel::Fatal},
    {.alias = "panic", .level = LogLevel::Fatal},
    {.alias = "alert", .level = LogLevel::Fatal},
    {.alias = "fault", .level = LogLevel::Fatal},
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
    // Overrides win over the built-in table so callers can remap any alias.
    for (const auto &[alias, canonicalName] : overrides)
    {
        if (!internal::EqualsIgnoreCaseAscii(bytes, alias))
        {
            continue;
        }
        // Resolve canonical side through the alias table (accepts any
        // casing). Invalid canonical strings fall through to the next
        // override and eventually to the built-in lookup, per header.
        if (const auto resolved = ParseLevelName(canonicalName); resolved.has_value())
        {
            return resolved;
        }
    }
    return ParseLevelName(bytes);
}

} // namespace loglib
