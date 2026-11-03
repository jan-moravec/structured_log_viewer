/**
 * @brief Canonical log levels and name parsing.
 *
 * Level columns are sorted by severity, not by text. This header defines the canonical levels, their order, and the
 * mapping from the many spellings found in logs, such as warn or WARNING, to one level.
 *
 * This is part of the core module.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace loglib
{

/**
 * @brief Canonical log level. Values go up with severity and are the sort rank of Level columns. Unknown means the row
 * has no level and sorts first.
 */
enum class LogLevel : uint8_t
{
    Unknown = 0,
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
};

/** @brief Number of levels from Trace to Fatal. Unknown is not counted. */
inline constexpr size_t CANONICAL_LEVEL_COUNT = 6;

/** @brief The name of a level, such as "Info". Unknown gives "Unknown". */
[[nodiscard]] std::string_view CanonicalLevelName(LogLevel level) noexcept;

/**
 * @brief Maps text to a level using the built-in alias table.
 *
 * Matching ignores case. The aliases are:
 *   - Trace: trace, trc, t, finer, finest, silly
 *   - Debug: debug, dbg, d, verbose, vrb, v, fine
 *   - Info:  info, inf, i, information, informational, notice
 *   - Warn:  warn, wrn, w, warning
 *   - Error: error, err, e, severe
 *   - Fatal: fatal, ftl, f, critical, crit, emerg, emergency, panic, alert, fault
 *
 * Verbose, vrb and v map to Debug, not Trace.
 *
 * Numeric levels such as those of Bunyan, Pino and syslog are not built in, because the conventions disagree. Map them
 * per column with levelMapping.
 *
 * Returns std::nullopt when no alias matches.
 */
[[nodiscard]] std::optional<LogLevel> ParseLevelName(std::string_view bytes) noexcept;

/**
 * @brief Maps text to a level, checking the overrides before the built-in table.
 *
 * Each override pairs an alias with the name of a level, which must be Trace, Debug, Info, Warn, Error or Fatal in any
 * case. An override with any other level name is ignored and its alias falls through to the built-in table. There is
 * no override that means "no level". To keep a value out of the level column, pin the column to Type::Enumeration.
 *
 * Returns std::nullopt when neither the overrides nor the built-in table match.
 */
[[nodiscard]] std::optional<LogLevel> ResolveLevel(
    std::string_view bytes, std::span<const std::pair<std::string, std::string>> overrides
) noexcept;

} // namespace loglib
