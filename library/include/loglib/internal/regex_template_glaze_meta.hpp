#pragma once

#include "loglib/regex_templates.hpp"

#include <array>
#include <glaze/glaze.hpp>

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
    // Keys pair positionally with values: reading is case-sensitive
    // and matches the wire spelling. Older files (pre-feature) miss
    // the field entirely and default to `None` via the struct's
    // in-class initialiser.
    static constexpr std::array keys{"none", "indented", "untilNextHeader"};
    static constexpr std::array value{None, Indented, UntilNextHeader};
};

template <> struct glz::meta<loglib::RegexTemplate>
{
    using T = loglib::RegexTemplate;
    // `error_on_unknown_keys=false` is set on the reader options in
    // `library/src/regex_templates.cpp` (via `LOG_CONFIG_OPTS`), so
    // older builds tolerate new fields and pre-feature JSON files
    // load with `continuationMode = None` + empty `headerAnchor`
    // via the struct defaults. Append new fields at the end.
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
