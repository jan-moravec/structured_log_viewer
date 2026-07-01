#pragma once

#include "loglib/regex_templates.hpp"

#include <glaze/glaze.hpp>

// Glaze meta-specialisation for `loglib::RegexTemplate`. Kept out
// of the public header so consumers don't pull in `<glaze/glaze.hpp>`.
// Mirrors `theme_glaze_meta.hpp`.
//
// `value` is a slot name required by Glaze reflection, so it keeps
// its lowercase name.
// NOLINTBEGIN(readability-identifier-naming)

template <> struct glz::meta<loglib::RegexTemplate>
{
    using T = loglib::RegexTemplate;
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
        &T::description
    );
};
// NOLINTEND(readability-identifier-naming)
