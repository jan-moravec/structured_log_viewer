#pragma once

#include "loglib/log_configuration.hpp"

#include <glaze/glaze.hpp>

#include <array>

// Glaze meta-specialisations for `LogConfiguration` enums. String-based
// JSON encoding keeps saved configs stable and human-readable. The on-disk
// `keys` stay as the original lowerCamelCase even though the C++
// enumerators are UpperCamelCase, so existing configurations keep working.
// Out-of-line so the public header does not pull in `<glaze/glaze.hpp>`.
// (`floating` rather than `double` since `double` is a reserved keyword.)
//
// `keys` and `value` are slot names mandated by glaze's reflection: its
// template machinery looks them up by exact name, so they cannot adopt the
// project-wide UPPER_CASE constexpr convention without breaking JSON I/O.
// NOLINTBEGIN(readability-identifier-naming)

template <> struct glz::meta<loglib::LogConfiguration::Type>
{
    using enum loglib::LogConfiguration::Type;
    static constexpr std::array keys{
        "unknown", "any", "string", "boolean", "integer", "floating", "number", "time", "enumeration", "level"
    };
    static constexpr std::array value{
        Unknown, Any, String, Boolean, Integer, Floating, Number, Time, Enumeration, Level
    };
};

template <> struct glz::meta<loglib::LogConfiguration::LogFilter::Type>
{
    using enum loglib::LogConfiguration::LogFilter::Type;
    static constexpr std::array keys{"string", "time", "enumeration", "number", "boolean"};
    static constexpr std::array value{String, Time, Enumeration, Number, Boolean};
};

template <> struct glz::meta<loglib::LogConfiguration::LogFilter::Match>
{
    using enum loglib::LogConfiguration::LogFilter::Match;
    static constexpr std::array keys{"exactly", "contains", "regularExpression", "wildcard"};
    static constexpr std::array value{Exactly, Contains, RegularExpression, Wildcard};
};
// NOLINTEND(readability-identifier-naming)
