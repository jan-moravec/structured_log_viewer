/**
 * @brief Colors and styles for displaying logs.
 *
 * A Theme describes the colors of log levels, table rows, window chrome, anchors and highlight slots, plus per-level
 * overrides. It is a plain value that can be loaded from and saved to JSON. The library only stores themes. Applying
 * them is up to the application.
 *
 * This is part of the support module.
 */
#pragma once

#include "loglib/core/log_level.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{

/**
 * @brief Says whether a theme is made for a light or a dark desktop. Applications use it to choose a theme that fits
 * the system setting.
 */
enum class ThemeKind : uint8_t
{
    Light = 0,
    Dark,
};

/**
 * @brief How rows of one log level look.
 *
 * Every color is optional, and a missing one is taken from the application's palette. Colors are written as #RRGGBB or
 * #AARRGGBB text, which keeps this header free of any GUI library.
 */
struct LevelStyle
{
    std::optional<std::string> foreground;
    std::optional<std::string> background;
    bool bold = false;
    bool italic = false;

    friend bool operator==(const LevelStyle &, const LevelStyle &) = default;
};

/**
 * @brief The colors of the table itself. Every color is optional, and a missing one keeps the style default.
 *
 * Set background whenever alternateRowBackground is set. The base color of the toolkit does not change with the theme,
 * so overriding only one of them gives rows that do not match in light and dark themes.
 *
 * The main log table does not alternate its rows, because the level colors already separate them. The alternating color
 * is for other tables.
 */
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

/**
 * @brief The colors of the window around the table: background, text, buttons and tooltips.
 *
 * Every color is optional. A missing one gets a default for the theme kind that matches the built-in themes.
 *
 * For disabled controls each text color is blended toward the color behind it, for example text toward the background
 * and window text toward the window. Override both colors of a pair together to get predictable disabled colors.
 */
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

/**
 * @brief Application-wide style settings. Every one is optional, and a missing one keeps what the application started
 * with.
 */
struct AppStyle
{
    /** @brief The name of a widget style, such as fusion or windows. */
    std::optional<std::string> qtStyle;
    std::optional<std::string> fontFamily;
    std::optional<int> fontSize;

    friend bool operator==(const AppStyle &, const AppStyle &) = default;
};

/**
 * @brief One color pair of the highlight palette. Both colors are optional, so a slot can fix one or both. Colors are
 * written as #RRGGBB or #AARRGGBB. A missing slot uses the application's built-in palette.
 */
struct HighlightSlot
{
    std::optional<std::string> foreground;
    std::optional<std::string> background;

    friend bool operator==(const HighlightSlot &, const HighlightSlot &) = default;
};

/**
 * @brief How one level is drawn when the level column shows icons: an icon, and a rounded background behind it. Every
 * field is optional. Colors are written as #RRGGBB or #AARRGGBB.
 */
struct LevelDisplayOverride
{
    /** @brief The icon, as a resource path or a path relative to the theme folder. Without it the level has no icon. */
    std::optional<std::string> icon;

    /**
     * @brief The color of the rounded background behind the icon. Without it there is none, and the icon is drawn over
     * the row background.
     */
    std::optional<std::string> pillBackground;

    /** @brief The color of the icon. Without it the level style foreground is used, and then the window text color. */
    std::optional<std::string> pillForeground;

    friend bool operator==(const LevelDisplayOverride &, const LevelDisplayOverride &) = default;
};

/**
 * @brief Switches the level column of a theme to icons.
 *
 * Whether Theme::levelColumnOverride has a value is the one switch every consumer reads. The fields only add to the
 * normal behavior, and a missing field keeps the usual header of the column.
 */
struct LevelColumnOverride
{
    /**
     * @brief The text of the column header. An empty string gives a blank header. Without a value the column's own
     * header is used.
     */
    std::optional<std::string> header;

    /**
     * @brief An icon for the column header, as a resource path or a path relative to the theme folder. A warning icon
     * or a filter icon takes precedence when one is shown.
     */
    std::optional<std::string> headerIcon;

    /**
     * @brief How each level is drawn, keyed by level name from Trace to Fatal, or Unknown.
     *
     * A level without an entry shows a blank cell. Keys that are not level names are reported by
     * WarnOnUnknownLevelKeys.
     */
    std::map<std::string, LevelDisplayOverride> levels;

    friend bool operator==(const LevelColumnOverride &, const LevelColumnOverride &) = default;
};

/**
 * @brief A complete theme: level styles, table and window colors, palettes and the level column.
 *
 * In the saved JSON the keys are the member names in lowerCamelCase. The keys of levels must be level names from Trace
 * to Fatal, or Unknown, and other keys are ignored.
 */
struct Theme
{
    /**
     * @brief The name of the theme, such as Dark. Names are case-sensitive, and a user theme with the name of a
     * built-in one replaces it.
     */
    std::string name;

    /** @brief Whether the theme is light or dark, so that automatic switching knows where it fits. */
    ThemeKind kind = ThemeKind::Light;

    /** @brief The row style of each level, keyed by level name. Used unless the user turned on high contrast levels. */
    std::map<std::string, LevelStyle> levels;

    /**
     * @brief Stronger level styles for users who turn on high contrast levels.
     *
     * A level missing here uses its entry in levels. An empty map means the theme has no high contrast variant, and the
     * option is shown as unavailable.
     */
    std::map<std::string, LevelStyle> levelsHighContrast;

    TableStyle table;
    ChromeStyle chrome;
    AppStyle app;

    /**
     * @brief Up to ANCHOR_PALETTE_SIZE colors, written as #RRGGBB, for anchored rows. Missing slots use the
     * application's built-in palette.
     */
    std::vector<std::string> anchorPalette;

    /**
     * @brief Up to HIGHLIGHT_PALETTE_SIZE color pairs for highlight rules.
     *
     * HighlightRule refers to them by number starting at 1, and 0 means the normal color. Missing or empty slots use
     * the built-in palette.
     */
    std::vector<HighlightSlot> highlightPalette;

    /**
     * @brief Icon settings for the level column. Without a value the column shows plain text. A user setting for icons
     * must also be on.
     */
    std::optional<LevelColumnOverride> levelColumnOverride;

    friend bool operator==(const Theme &, const Theme &) = default;
};

/** @brief The number of anchor colors. It is defined here so that tests that do not use Qt can use it. */
inline constexpr std::size_t ANCHOR_PALETTE_SIZE = 8;

/**
 * @brief The number of highlight color pairs. HighlightRule refers to them by number starting at 1, and 0 means no
 * color is chosen.
 */
inline constexpr std::size_t HIGHLIGHT_PALETTE_SIZE = 16;

/** @brief The style of a level, or an empty style if the theme has none for it. */
[[nodiscard]] LevelStyle StyleForLevel(const Theme &theme, LogLevel level);

/**
 * @brief Like the first overload, but with useHighContrast set it looks in levelsHighContrast first and then in levels.
 *
 * A level style is taken as a whole from one of them, so foreground, background, bold and italic are never mixed.
 */
[[nodiscard]] LevelStyle StyleForLevel(const Theme &theme, LogLevel level, bool useHighContrast);

/** @brief Reads a theme from JSON. The kind must be light or dark. Throws std::runtime_error on any error. */
[[nodiscard]] Theme ParseTheme(std::string_view content);

/** @brief Writes a theme as indented JSON. Throws std::runtime_error if encoding fails. */
[[nodiscard]] std::string SerializeTheme(const Theme &theme);

} // namespace loglib
