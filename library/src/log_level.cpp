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
///
/// Coverage notes:
///   - Single-letter aliases (`t`/`d`/`v`/`i`/`w`/`e`/`f`) cover the
///     Android logger / glog / many embedded logger conventions. They
///     are safe to include here because dictionary-content matching
///     runs only after `IsLogLevelKey` already gates the column.
///   - `v` / `vrb` / `verbose` all map to `Debug` (not `Trace`) for
///     back-compat with the existing `verbose -> Debug` entry; Serilog
///     and Android treat their Verbose level as below Debug, but
///     flipping it now would break saved configs that rely on the
///     prior mapping.
///   - Numeric levels (Bunyan/Pino `10/20/30/40/50/60`, syslog `0..7`)
///     are intentionally absent: the two conventions disagree and
///     numeric JSON values typically arrive as `Int64`, not strings,
///     so they never enter the enum dictionary. Users who need them
///     can map per-column via `levelMapping`.
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
