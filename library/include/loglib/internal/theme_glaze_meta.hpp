#pragma once

#include "loglib/theme.hpp"

#include <glaze/glaze.hpp>

#include <array>

// Glaze meta-specialisations for `loglib::Theme` and friends. Out-of-
// line so the public header does not pull in `<glaze/glaze.hpp>`.
//
// `keys` and `value` are slot names mandated by glaze's reflection: its
// template machinery looks them up by exact name, so they cannot adopt the
// project-wide UPPER_CASE constexpr convention without breaking JSON I/O.
// NOLINTBEGIN(readability-identifier-naming)

template <> struct glz::meta<loglib::ThemeKind>
{
    using enum loglib::ThemeKind;
    static constexpr std::array keys{"light", "dark"};
    static constexpr std::array value{Light, Dark};
};

template <> struct glz::meta<loglib::LevelStyle>
{
    using T = loglib::LevelStyle;
    static constexpr auto value =
        object("foreground", &T::foreground, "background", &T::background, "bold", &T::bold, "italic", &T::italic);
};

template <> struct glz::meta<loglib::TableStyle>
{
    using T = loglib::TableStyle;
    static constexpr auto value = object(
        "background",
        &T::background,
        "alternateRowBackground",
        &T::alternateRowBackground,
        "selectionBackground",
        &T::selectionBackground,
        "selectionForeground",
        &T::selectionForeground,
        "headerBackground",
        &T::headerBackground,
        "headerForeground",
        &T::headerForeground
    );
};

template <> struct glz::meta<loglib::ChromeStyle>
{
    using T = loglib::ChromeStyle;
    static constexpr auto value = object(
        "window",
        &T::window,
        "windowText",
        &T::windowText,
        "text",
        &T::text,
        "button",
        &T::button,
        "buttonText",
        &T::buttonText,
        "placeholderText",
        &T::placeholderText,
        "toolTipBase",
        &T::toolTipBase,
        "toolTipText",
        &T::toolTipText
    );
};

template <> struct glz::meta<loglib::AppStyle>
{
    using T = loglib::AppStyle;
    static constexpr auto value =
        object("qtStyle", &T::qtStyle, "fontFamily", &T::fontFamily, "fontSize", &T::fontSize);
};

template <> struct glz::meta<loglib::Theme>
{
    using T = loglib::Theme;
    static constexpr auto value = object(
        "name",
        &T::name,
        "kind",
        &T::kind,
        "levels",
        &T::levels,
        "table",
        &T::table,
        "chrome",
        &T::chrome,
        "app",
        &T::app
    );
};
// NOLINTEND(readability-identifier-naming)
