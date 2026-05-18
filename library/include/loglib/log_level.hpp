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

/// Canonical log level. Values are ordered by severity and used as the
/// sort rank for `Type::Level` columns. `Unknown` means "no level
/// information" and sorts before `Trace`.
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

/// Number of canonical levels (Trace..Fatal); excludes `Unknown`.
inline constexpr size_t CANONICAL_LEVEL_COUNT = 6;

/// Display name for @p level, e.g. `"Info"`. Returns `"Unknown"` for the sentinel.
[[nodiscard]] std::string_view CanonicalLevelName(LogLevel level) noexcept;

/// Map @p bytes to a canonical level via the built-in alias table.
///
/// Recognised aliases (case-insensitive):
///   - Trace: trace, trc, t, finer, finest, silly
///   - Debug: debug, dbg, d, verbose, vrb, v, fine
///   - Info:  info, inf, i, information, informational, notice
///   - Warn:  warn, wrn, w, warning
///   - Error: error, err, e, severe
///   - Fatal: fatal, ftl, f, critical, crit, emerg, emergency, panic, alert, fault
///
/// `verbose`/`vrb`/`v` map to `Debug` (not `Trace`) for backwards
/// compatibility with saved configs.
///
/// Numeric levels (Bunyan/Pino, syslog) are not built in: conventions
/// disagree, and numeric JSON values usually arrive as `Int64` and never
/// reach the enum dictionary. Map them per-column via `levelMapping`.
///
/// Returns `std::nullopt` when no alias matches.
[[nodiscard]] std::optional<LogLevel> ParseLevelName(std::string_view bytes) noexcept;

/// Map @p bytes to a level, checking @p overrides before the built-in table.
///
/// Each override is `(alias, canonicalName)`; the canonical side must
/// spell one of `Trace`/`Debug`/`Info`/`Warn`/`Error`/`Fatal`
/// (case-insensitive). Overrides whose canonical side does not match
/// (typos, `"Unknown"`, etc.) are silently ignored and the alias falls
/// through to the built-in table.
///
/// There is no sentinel for "treat as no level"; to suppress an alias,
/// remove it from the column data or pin the column to `Type::Enumeration`.
///
/// Returns `std::nullopt` when neither overrides nor the built-in table match.
[[nodiscard]] std::optional<LogLevel> ResolveLevel(
    std::string_view bytes, std::span<const std::pair<std::string, std::string>> overrides
) noexcept;

} // namespace loglib
