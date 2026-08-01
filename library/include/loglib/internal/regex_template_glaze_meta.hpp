#pragma once

#include "loglib/regex_templates.hpp"

#include <glaze/glaze.hpp>
#include <array>

// Glaze meta-specialisation for `loglib::RegexTemplate`. Kept out
// of the public header so consumers don't pull in
// `<glaze/glaze.hpp>`. Mirrors `theme_glaze_meta.hpp`.
//
// `value` is a slot name required by Glaze reflection, so keeps
// its lowercase name.
// NOLINTBEGIN(readability-identifier-naming)

template <> struct glz::meta<loglib::ContinuationMode>
{
    using enum loglib::ContinuationMode;
    // Keys pair positionally with values and preserve case-sensitive
    // wire spellings.
    static constexpr std::array keys{"none", "indented", "untilNextHeader"};
    static constexpr std::array value{None, Indented, UntilNextHeader};
};

template <> struct glz::meta<loglib::RegexTemplate>
{
    using T = loglib::RegexTemplate;
    // Missing fields retain the defaults declared on `RegexTemplate`.
    static constexpr auto value = object(
        "name",
        &T::name,
        "pattern",
        &T::pattern,
        "sampleLines",
        &T::sampleLines,
        "autoDetect",
        &T::autoDetect,
        "priority",
        &T::priority,
        "description",
        &T::description,
        "continuationMode",
        &T::continuationMode,
        "headerAnchor",
        &T::headerAnchor
    );
};
// NOLINTEND(readability-identifier-naming)
