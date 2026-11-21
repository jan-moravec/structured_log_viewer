/**
 * @brief JSON field names for regex templates.
 *
 * This header tells the JSON library how to read and write RegexTemplate. It is only included where templates are
 * serialized.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/config/internal/log_configuration_glaze_meta.hpp"
#include "loglib/parse/regex_templates.hpp"

#include <glaze/glaze.hpp>
#include <array>

// JSON field names for RegexTemplate, like theme_glaze_meta.hpp does for Theme. They live in this header so the public
// header does not include the JSON library. The name value is required by the library's reflection, so the naming check
// is turned off here.
// NOLINTBEGIN(readability-identifier-naming)

template <> struct glz::meta<loglib::RegexTemplate>
{
    using T = loglib::RegexTemplate;

    // A template file keeps its flat keys. They map onto the nested settings of the template.
    static constexpr auto readPattern = [](T &self, std::string value) { self.regex.pattern = std::move(value); };
    static constexpr auto writePattern = [](const T &self) { return self.regex.pattern; };
    static constexpr auto readContinuation = [](T &self, loglib::ContinuationMode value) {
        self.regex.continuation = value;
    };
    static constexpr auto writeContinuation = [](const T &self) { return self.regex.continuation; };
    static constexpr auto readHeaderAnchor = [](T &self, std::string value) {
        self.regex.headerAnchor = std::move(value);
    };
    static constexpr auto writeHeaderAnchor = [](const T &self) { return self.regex.headerAnchor; };

    // A field missing from the file keeps the default that RegexTemplate declares.
    static constexpr auto value = object(
        "name",
        &T::name,
        "pattern",
        glz::custom<readPattern, writePattern>,
        "sampleLines",
        &T::sampleLines,
        "autoDetect",
        &T::autoDetect,
        "priority",
        &T::priority,
        "description",
        &T::description,
        "continuationMode",
        glz::custom<readContinuation, writeContinuation>,
        "headerAnchor",
        glz::custom<readHeaderAnchor, writeHeaderAnchor>
    );
};
// NOLINTEND(readability-identifier-naming)
