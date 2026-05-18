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
    {"trace", LogLevel::Trace},
    {"trc", LogLevel::Trace},
    {"t", LogLevel::Trace},
    {"finer", LogLevel::Trace},
    {"finest", LogLevel::Trace},
    {"silly", LogLevel::Trace},
    // Debug
    {"debug", LogLevel::Debug},
    {"dbg", LogLevel::Debug},
    {"d", LogLevel::Debug},
    {"verbose", LogLevel::Debug},
    {"vrb", LogLevel::Debug},
    {"v", LogLevel::Debug},
    {"fine", LogLevel::Debug},
    // Info
    {"info", LogLevel::Info},
    {"inf", LogLevel::Info},
    {"i", LogLevel::Info},
    {"information", LogLevel::Info},
    {"informational", LogLevel::Info},
    {"notice", LogLevel::Info},
    // Warn
    {"warn", LogLevel::Warn},
    {"wrn", LogLevel::Warn},
    {"w", LogLevel::Warn},
    {"warning", LogLevel::Warn},
    // Error
    {"error", LogLevel::Error},
    {"err", LogLevel::Error},
    {"e", LogLevel::Error},
    {"severe", LogLevel::Error},
    // Fatal
    {"fatal", LogLevel::Fatal},
    {"ftl", LogLevel::Fatal},
    {"f", LogLevel::Fatal},
    {"critical", LogLevel::Fatal},
    {"crit", LogLevel::Fatal},
    {"emerg", LogLevel::Fatal},
    {"emergency", LogLevel::Fatal},
    {"panic", LogLevel::Fatal},
    {"alert", LogLevel::Fatal},
    {"fault", LogLevel::Fatal},
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
