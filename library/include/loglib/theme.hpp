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

/// Per-level icon + pill spec for the level column override.
/// All fields optional so a level can be left at "use defaults".
/// Colours are `#RRGGBB` (or `#AARRGGBB`) strings to keep this
/// header Qt-free, mirroring `LevelStyle`.
struct LevelDisplayOverride
{
    /// Icon path. `:/...` (Qt resource) or relative-to-theme-dir.
    /// Absent ⇒ no icon for this level; the cell paints blank under
    /// the delegate's pill (or nothing when no pill is configured).
    std::optional<std::string> icon;

    /// Rounded-rect pill background. Absent ⇒ no pill (the icon
    /// paints directly over the cell, on top of whatever
    /// `BackgroundRole` filled the row with).
    std::optional<std::string> pillBackground;

    /// Tint for the icon glyph. Absent ⇒ fall back to
    /// `LevelStyle::foreground` for the level, then to the
    /// palette's `WindowText`. Distinct from `LevelStyle::foreground`
    /// (which colours the cell text on non-icon rows).
    std::optional<std::string> pillForeground;

    friend bool operator==(const LevelDisplayOverride &, const LevelDisplayOverride &) = default;
};

/// Opts a theme into icon-rendering for the level column.
/// `Theme::levelColumnOverride.has_value()` is the single switch
/// every consumer reads -- no scattered "does any level have an
/// icon?" probes.
///
/// Header chrome is purely additive: when a field is absent the
/// app falls back to whatever the column is already configured to
/// show (`LogConfiguration::Column::header` and the existing
/// warning > funnel decoration priority). The theme override is
/// the *opt-in*, not a default replacement.
struct LevelColumnOverride
{
    /// Header text override. Set ⇒ use this string (`""` ⇒ blank
    /// header text). Absent ⇒ fall back to `Column::header`.
    std::optional<std::string> header;

    /// Header icon override (Qt resource path or
    /// relative-to-theme-dir). Set ⇒ render this icon as the
    /// level-column identity icon (warning + funnel still take
    /// precedence when active). Absent ⇒ no identity icon;
    /// today's `DecorationRole` priority (warning > funnel >
    /// nothing) applies.
    std::optional<std::string> headerIcon;

    /// Per-level icon + pill specs. Keyed by canonical level name
    /// (`"Trace"`..`"Fatal"`, or `"Unknown"`) for wire-format
    /// symmetry with `Theme::levels`. `ThemeControl::BuildStyleCache`
    /// projects this into a `LogLevel`-indexed `std::array` for
    /// O(1) paint-path lookup; non-canonical keys warn via the
    /// shared `WarnOnUnknownLevelKeys` machinery. Missing keys
    /// mean "no icon for this level" -- the cell shows blank.
    std::map<std::string, LevelDisplayOverride> levels;

    friend bool operator==(const LevelColumnOverride &, const LevelColumnOverride &) = default;
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
    ///
    /// These are the **subtle defaults** -- the showcase look used
    /// when the user pref `ui/highContrastLevels` is off (the
    /// default). When the pref is on, `levelsHighContrast` overrides
    /// per-level.
    std::map<std::string, LevelStyle> levels;

    /// Optional per-level overrides applied on top of `levels` when
    /// the user pref `ui/highContrastLevels` is on. Sparse: missing
    /// keys fall back to `levels[key]`, so a theme can boost only
    /// `Warn`/`Error`/`Fatal` and keep `Trace`/`Debug` identical.
    ///
    /// Keys are the canonical level names (`Trace`..`Fatal`, or
    /// `Unknown`). Non-canonical keys warn via the shared
    /// `WarnOnUnknownLevelKeys` machinery in `theme_control.cpp`.
    ///
    /// An empty map means the theme opts out of the toggle; the
    /// Preferences checkbox disables itself in that case (same UX
    /// as the pills toggle when `levelColumnOverride` is absent).
    std::map<std::string, LevelStyle> levelsHighContrast;

    TableStyle table;
    ChromeStyle chrome;
    AppStyle app;

    /// Up to `ANCHOR_PALETTE_SIZE` `#RRGGBB` entries for anchor
    /// rows. Missing slots fall back to the app's built-in palette
    /// in `ThemeControl::AnchorBrushFor`.
    std::vector<std::string> anchorPalette;

    /// `nullopt` ⇒ level column renders as plain text (today's
    /// behaviour). Set ⇒ icon mode, gated additionally by the
    /// `ui/showLevelIcons` user preference. Cell + header
    /// resolution rules live in `app/include/log_model.hpp`
    /// `headerData` / `data` (see plan section 5 for the table).
    std::optional<LevelColumnOverride> levelColumnOverride;

    friend bool operator==(const Theme &, const Theme &) = default;
};

/// Number of anchor colour slots (matches the `Ctrl+1..8` hotkey
/// block). Lives here so `loglib_test` can use it without Qt.
inline constexpr std::size_t ANCHOR_PALETTE_SIZE = 8;

/// Returns the style for @p level, or a default-constructed
/// `LevelStyle` when the theme has no entry for it.
[[nodiscard]] LevelStyle StyleForLevel(const Theme &theme, LogLevel level);

/// Same as `StyleForLevel(theme, level)` when @p useHighContrast is
/// false. When true, looks up @p level in `theme.levelsHighContrast`
/// first and falls back to `theme.levels` if the override is absent.
/// Sparse overrides are resolved at the *whole-`LevelStyle` granularity*:
/// if `levelsHighContrast[name]` exists, its fields (foreground,
/// background, bold, italic) replace the subtle entry entirely. This
/// matches how theme authors think about the two looks (hand-tuned
/// per level), and avoids surprising blends of subtle + loud fields.
[[nodiscard]] LevelStyle StyleForLevel(const Theme &theme, LogLevel level, bool useHighContrast);

/// Parse a theme from JSON. Throws `std::runtime_error` on any
/// parse error. `kind` must be `"light"` or `"dark"`.
[[nodiscard]] Theme ParseTheme(std::string_view content);

/// Serialise @p theme to pretty-printed JSON. Throws
/// `std::runtime_error` on encode failure.
[[nodiscard]] std::string SerializeTheme(const Theme &theme);

} // namespace loglib
