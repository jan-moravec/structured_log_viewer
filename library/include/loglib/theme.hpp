#pragma once

#include "loglib/log_level.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{

/// Marks a theme as intended for a light or dark desktop palette.
/// Used by the app's auto-switch to pick a sensible default.
enum class ThemeKind : uint8_t
{
    Light = 0,
    Dark,
};

/// Per-`LogLevel` row styling. All fields optional; missing colours
/// fall back to the active Qt palette. Colours are `#RRGGBB` (or
/// `#AARRGGBB`) strings to keep this header Qt-free.
struct LevelStyle
{
    std::optional<std::string> foreground;
    std::optional<std::string> background;
    bool bold = false;
    bool italic = false;

    friend bool operator==(const LevelStyle &, const LevelStyle &) = default;
};

/// Table chrome: base/alternate/selection/header colours. All
/// optional; absent fields keep the Qt style defaults.
///
/// Set `background` whenever `alternateRowBackground` is set --
/// `QPalette::Base` does not change with the theme, so a partial
/// override yields mismatched light/dark rows.
///
/// The main log table no longer alternates rows (per-level
/// colours already partition them); `alternateRowBackground`
/// still drives secondary tables like Record Details.
struct TableStyle
{
    std::optional<std::string> background;
    std::optional<std::string> alternateRowBackground;
    std::optional<std::string> selectionBackground;
    std::optional<std::string> selectionForeground;
    std::optional<std::string> headerBackground;
    std::optional<std::string> headerForeground;

    friend bool operator==(const TableStyle &, const TableStyle &) = default;
};

/// Window-level chrome (background, text, buttons, tooltips).
/// All optional; absent fields fall back to per-`ThemeKind`
/// defaults tuned to match the built-in presets.
///
/// For the `Disabled` colour group, each text role is blended
/// toward its backing surface (`text` toward `background`,
/// `windowText` toward `window`, etc.). Override both sides
/// together for predictable disabled-state colours.
struct ChromeStyle
{
    std::optional<std::string> window;
    std::optional<std::string> windowText;
    std::optional<std::string> text;
    std::optional<std::string> button;
    std::optional<std::string> buttonText;
    std::optional<std::string> placeholderText;
    std::optional<std::string> toolTipBase;
    std::optional<std::string> toolTipText;

    friend bool operator==(const ChromeStyle &, const ChromeStyle &) = default;
};

/// App-wide style overrides. All optional; absent fields keep
/// the process defaults captured at startup.
struct AppStyle
{
    /// e.g. "fusion", "windows". Resolved via `QStyleFactory::create`.
    std::optional<std::string> qtStyle;
    std::optional<std::string> fontFamily;
    std::optional<int> fontSize;

    friend bool operator==(const AppStyle &, const AppStyle &) = default;
};

/// Self-contained theme bundle. JSON keys mirror the field names
/// (lowerCamelCase). `levels` keys must be canonical level names
/// (`Trace`..`Fatal`, or `Unknown`); other keys are ignored.
struct Theme
{
    /// Display name, e.g. "Dark". Case-sensitive identity; user
    /// themes whose name matches a built-in shadow the built-in.
    std::string name;

    /// Auto-switch hint. User themes pick one so the auto-picker
    /// knows which slot they belong to.
    ThemeKind kind = ThemeKind::Light;

    /// Per-level row styles, keyed by canonical level name
    /// (`Trace`..`Fatal`) or `"Unknown"`. Other keys are ignored.
    std::map<std::string, LevelStyle> levels;

    TableStyle table;
    ChromeStyle chrome;
    AppStyle app;

    /// Up to `ANCHOR_PALETTE_SIZE` `#RRGGBB` entries for anchor
    /// rows. Missing slots fall back to the app's built-in palette
    /// in `ThemeControl::AnchorBrushFor`.
    std::vector<std::string> anchorPalette;

    friend bool operator==(const Theme &, const Theme &) = default;
};

/// Number of anchor colour slots (matches the `Ctrl+1..8` hotkey
/// block). Lives here so `loglib_test` can use it without Qt.
inline constexpr std::size_t ANCHOR_PALETTE_SIZE = 8;

/// Returns the style for @p level, or a default-constructed
/// `LevelStyle` when the theme has no entry for it.
[[nodiscard]] LevelStyle StyleForLevel(const Theme &theme, LogLevel level);

/// Parse a theme from JSON. Throws `std::runtime_error` on any
/// parse error. `kind` must be `"light"` or `"dark"`.
[[nodiscard]] Theme ParseTheme(std::string_view content);

/// Serialise @p theme to pretty-printed JSON. Throws
/// `std::runtime_error` on encode failure.
[[nodiscard]] std::string SerializeTheme(const Theme &theme);

} // namespace loglib
