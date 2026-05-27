#pragma once

#include "loglib/log_level.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace loglib
{

/// Whether a theme is intended for use on a light or a dark desktop
/// palette. Drives the auto-switch in the app's `ThemeControl`: the
/// app picks the first built-in `Light` theme on a bright system
/// palette, the first built-in `Dark` theme otherwise.
enum class ThemeKind : uint8_t
{
    Light = 0,
    Dark,
};

/// Per-`LogLevel` row styling. All fields are optional; missing
/// colors fall back to the active Qt palette, missing flags to
/// `false`. Colors are stored as raw `#RRGGBB` (or `#AARRGGBB`)
/// strings so the library has no Qt dependency; the app parses
/// them via `QColor::fromString`.
struct LevelStyle
{
    std::optional<std::string> foreground;
    std::optional<std::string> background;
    bool bold = false;
    bool italic = false;
};

/// Chrome around the rows: base background, alternating stripe,
/// selection rectangle, header. All optional; absent fields keep
/// the Qt style defaults.
///
/// `background` MUST be set alongside `alternateRowBackground`:
/// the QTableView default `background-color` comes from the
/// `QPalette::Base` role, which does not change when a theme
/// switches, so a theme that only customises the alternate
/// stripe ends up rendering mismatched light/dark rows when the
/// system palette doesn't match the theme kind.
struct TableStyle
{
    std::optional<std::string> background;
    std::optional<std::string> alternateRowBackground;
    std::optional<std::string> selectionBackground;
    std::optional<std::string> selectionForeground;
    std::optional<std::string> headerBackground;
    std::optional<std::string> headerForeground;
};

/// App-wide style fields that the theme bundle also owns (Qt style
/// name and base font). All optional; absent fields keep the
/// process-global Qt defaults set at startup.
struct AppStyle
{
    /// e.g. "fusion", "windows". Resolved via `QStyleFactory::create`.
    std::optional<std::string> qtStyle;
    std::optional<std::string> fontFamily;
    std::optional<int> fontSize;
};

/// One self-contained theme bundle. JSON keys mirror the C++
/// field names (lowerCamelCase). The `levels` map is keyed by the
/// canonical level name (`Trace`, `Debug`, `Info`, `Warn`,
/// `Error`, `Fatal`); unknown keys are ignored.
struct Theme
{
    /// Display name, e.g. "Dark". Matched case-sensitively as the
    /// stable theme identity; user themes whose name collides with
    /// a built-in shadow the built-in.
    std::string name;

    /// Auto-switch hint. Built-in `Light` is `Light`, built-in
    /// `Dark` is `Dark`; user themes pick one explicitly so the
    /// auto-picker has a slot to put them in.
    ThemeKind kind = ThemeKind::Light;

    /// Per-level row styles. Keys must spell a canonical level
    /// name (see `loglib::CanonicalLevelName`); unknown keys are
    /// silently ignored at apply time.
    std::map<std::string, LevelStyle> levels;

    TableStyle table;
    AppStyle app;
};

/// Lookup helper: returns the style for @p level, or a default-
/// constructed `LevelStyle` when the theme does not define one.
/// Centralises the canonical-name lookup so callers don't repeat
/// the `CanonicalLevelName` + `map::find` dance.
[[nodiscard]] LevelStyle StyleForLevel(const Theme &theme, LogLevel level) noexcept;

/// Parse a theme from JSON. Throws `std::runtime_error` on any
/// parse error. `kind` is decoded from the string value `"light"`
/// or `"dark"`; any other value fails the parse.
[[nodiscard]] Theme ParseTheme(std::string_view content);

/// Serialise @p theme to pretty-printed JSON. Throws
/// `std::runtime_error` if Glaze cannot encode the value
/// (shouldn't happen for a well-formed `Theme`).
[[nodiscard]] std::string SerializeTheme(const Theme &theme);

} // namespace loglib
