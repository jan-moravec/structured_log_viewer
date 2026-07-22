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

/// One entry in `Theme::highlightPalette`. Both fields optional
/// so a slot can pin one or both. Colours are `#RRGGBB` /
/// `#AARRGGBB` (Qt-free, like `LevelStyle`). Missing slots fall
/// back to the app's built-in palette in
/// `ThemeControl::HighlightBrushFor`.
struct HighlightSlot
{
    std::optional<std::string> foreground;
    std::optional<std::string> background;

    friend bool operator==(const HighlightSlot &, const HighlightSlot &) = default;
};

/// Per-level icon + pill spec. All fields optional so a level can
/// be left at the defaults. Colours are `#RRGGBB` / `#AARRGGBB`
/// strings (kept Qt-free like `LevelStyle`).
struct LevelDisplayOverride
{
    /// `:/...` qrc path or relative-to-theme-dir. Absent = no icon.
    std::optional<std::string> icon;

    /// Rounded-rect pill background. Absent = no pill (icon
    /// paints over the row's `BackgroundRole`).
    std::optional<std::string> pillBackground;

    /// Tint for the icon glyph. Absent = fall back to
    /// `LevelStyle::foreground` then to palette `WindowText`.
    std::optional<std::string> pillForeground;

    friend bool operator==(const LevelDisplayOverride &, const LevelDisplayOverride &) = default;
};

/// Opts a theme into icon-rendering for the level column.
/// `Theme::levelColumnOverride.has_value()` is the single switch
/// every consumer reads. All fields are additive: absent fields
/// fall back to today's `Column::header` / warning > funnel
/// header priority.
struct LevelColumnOverride
{
    /// Header text override. Set = use it (`""` = blank text).
    /// Absent = fall back to `Column::header`.
    std::optional<std::string> header;

    /// Header identity icon (qrc or relative-to-theme-dir).
    /// Warning + funnel still take precedence when firing.
    std::optional<std::string> headerIcon;

    /// Per-level icon + pill specs, keyed by canonical level name
    /// (`"Trace"`..`"Fatal"`, or `"Unknown"`). Missing keys = no
    /// icon for that level (cell shows blank). Non-canonical
    /// keys warn via `WarnOnUnknownLevelKeys`.
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

    /// Per-level row styles (subtle defaults). Keyed by canonical
    /// level name. Used when `ui/highContrastLevels` is off (the
    /// default); when on, `levelsHighContrast` overrides per-level.
    std::map<std::string, LevelStyle> levels;

    /// Loud overrides applied on top of `levels` when
    /// `ui/highContrastLevels` is on. Sparse: missing keys fall
    /// back to `levels[key]`. An empty map opts the theme out of
    /// the toggle (the Preferences checkbox greys out).
    std::map<std::string, LevelStyle> levelsHighContrast;

    TableStyle table;
    ChromeStyle chrome;
    AppStyle app;

    /// Up to `ANCHOR_PALETTE_SIZE` `#RRGGBB` entries for anchor
    /// rows. Missing slots fall back to the app's built-in palette
    /// in `ThemeControl::AnchorBrushFor`.
    std::vector<std::string> anchorPalette;

    /// Up to `HIGHLIGHT_PALETTE_SIZE` swatches for the highlight
    /// rules editor. Referenced 1-indexed by `HighlightRule`
    /// `foregroundIndex` / `backgroundIndex` (0 = inherit).
    /// Missing / empty slots fall back to the built-in palette.
    std::vector<HighlightSlot> highlightPalette;

    /// nullopt = plain-text level column. Set = icon mode (also
    /// gated on the `ui/showLevelIcons` user pref). Cell + header
    /// resolution rules live in `LogModel::data`/`headerData`.
    std::optional<LevelColumnOverride> levelColumnOverride;

    friend bool operator==(const Theme &, const Theme &) = default;
};

/// Number of anchor colour slots (matches the `Ctrl+1..8` hotkey
/// block). Lives here so `loglib_test` can use it without Qt.
inline constexpr std::size_t ANCHOR_PALETTE_SIZE = 8;

/// Highlight-palette slot count. Referenced 1-indexed by
/// `HighlightRule::foregroundIndex` / `backgroundIndex`
/// (0 = inherit).
inline constexpr std::size_t HIGHLIGHT_PALETTE_SIZE = 16;

/// Returns the style for @p level, or a default-constructed
/// `LevelStyle` when the theme has no entry for it.
[[nodiscard]] LevelStyle StyleForLevel(const Theme &theme, LogLevel level);

/// When @p useHighContrast is true, look up @p level in
/// `theme.levelsHighContrast` first and fall back to
/// `theme.levels` if absent. Override is whole-`LevelStyle`
/// granular (fg, bg, bold, italic replace as a set) to avoid
/// surprising blends.
[[nodiscard]] LevelStyle StyleForLevel(const Theme &theme, LogLevel level, bool useHighContrast);

/// Parse a theme from JSON. Throws `std::runtime_error` on any
/// parse error. `kind` must be `"light"` or `"dark"`.
[[nodiscard]] Theme ParseTheme(std::string_view content);

/// Serialise @p theme to pretty-printed JSON. Throws
/// `std::runtime_error` on encode failure.
[[nodiscard]] std::string SerializeTheme(const Theme &theme);

} // namespace loglib
