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
///   - Trace: trace, trc, t, finer, finest, silly
///   - Debug: debug, dbg, d, verbose, vrb, v, fine
///   - Info:  info, inf, i, information, informational, notice
///   - Warn:  warn, wrn, w, warning
///   - Error: error, err, e, severe
///   - Fatal: fatal, ftl, f, critical, crit, emerg, emergency, panic, alert, fault
///
/// `verbose` / `vrb` / `v` map to `Debug` (not `Trace`) for back-compat
/// with the original mapping; Serilog and Android treat their Verbose
/// level as below Debug, but flipping this now would break saved
/// configs that depend on the prior behaviour.
///
/// Numeric-string levels (e.g. Bunyan/Pino `10/20/30/40/50/60`, syslog
/// `0..7`) are intentionally *not* in the built-in table -- the two
/// conventions disagree and numeric JSON values usually arrive as
/// `Int64`, which never enters the enum dictionary. Map them per-column
/// via `levelMapping` if you need them.
///
/// Returns `std::nullopt` if @p bytes does not match any recognised alias.
[[nodiscard]] std::optional<LogLevel> ParseLevelName(std::string_view bytes) noexcept;

/// Per-column alias overrides: `(alias, canonicalName)` pairs. Canonical
/// names are matched against `CanonicalLevelName` (case-insensitive); a
/// pair whose canonical side is not one of `Trace`, `Debug`, `Info`,
/// `Warn`, `Error`, or `Fatal` (notably including `Unknown` and any
/// misspelled name) is silently ignored, and the matching alias falls
/// through to the built-in table. Aliases are matched case-
/// insensitively. Overrides take precedence over the built-in table.
///
/// To suppress an alias entirely (rather than remap it) the caller
/// should either remove it from the column's data or pin the column to
/// `Type::Enumeration`; there is no sentinel for "treat as no level".
///
/// @p bytes is the raw user string to map. Returns `std::nullopt` if no
/// override and no built-in alias matches.
[[nodiscard]] std::optional<LogLevel> ResolveLevel(
    std::string_view bytes, std::span<const std::pair<std::string, std::string>> overrides
) noexcept;

} // namespace loglib
