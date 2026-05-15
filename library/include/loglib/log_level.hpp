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

/// Canonical log level. The ordered values are the sort rank used for
/// `Type::Level` columns (lower = less severe). `Unknown` is the sentinel
/// for "no level information"; it sorts before `Trace` in ascending order.
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

/// Number of canonical levels excluding `Unknown` (Trace..Fatal).
inline constexpr size_t CANONICAL_LEVEL_COUNT = 6;

/// Stable display name for @p level. Returns `"Unknown"` for the
/// sentinel; otherwise one of `Trace`/`Debug`/`Info`/`Warn`/`Error`/`Fatal`.
[[nodiscard]] std::string_view CanonicalLevelName(LogLevel level) noexcept;

/// Case-insensitive lookup against the built-in alias table.
///
/// Recognised aliases (case-insensitive):
///   - Trace: trace, trc
///   - Debug: debug, dbg, verbose
///   - Info:  info, information, informational, notice
///   - Warn:  warn, warning
///   - Error: error, err, severe
///   - Fatal: fatal, critical, crit, emerg, emergency, panic, alert
///
/// Returns `std::nullopt` if @p bytes does not match any recognised alias.
[[nodiscard]] std::optional<LogLevel> ParseLevelName(std::string_view bytes) noexcept;

/// Per-column alias overrides: `(alias, canonicalName)` pairs. Canonical
/// names are matched against `CanonicalLevelName` (case-insensitive); a
/// pair whose canonical side is not a recognised level name is silently
/// ignored. Aliases are matched case-insensitively. Overrides take
/// precedence over the built-in table.
///
/// @p bytes is the raw user string to map. Returns `std::nullopt` if no
/// override and no built-in alias matches.
[[nodiscard]] std::optional<LogLevel> ResolveLevel(
    std::string_view bytes, std::span<const std::pair<std::string, std::string>> overrides
) noexcept;

} // namespace loglib
