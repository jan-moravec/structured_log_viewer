/**
 * @brief JSON field names for regex templates.
 *
 * This header tells the JSON library how to read and write RegexTemplate. It is only included where templates are
 * serialized.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/parse/regex_templates.hpp"
#include "loglib/schema/internal/log_configuration_glaze_meta.hpp"

#include <glaze/glaze.hpp>
#include <array>

// JSON field names for RegexTemplate, like theme_glaze_meta.hpp does for Theme. They live in this header so the public
// header does not include the JSON library. The name value is required by the library's reflection, so the naming check
// is turned off here.
// NOLINTBEGIN(readability-identifier-naming)

template <> struct glz::meta<loglib::RegexTemplate>
{
    using T = loglib::RegexTemplate;
    // A field missing from the file keeps the default that RegexTemplate declares.
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
