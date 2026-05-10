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

template <> struct glz::meta<loglib::LogConfiguration::Type>
{
    using enum loglib::LogConfiguration::Type;
    static constexpr std::array keys{
        "unknown", "any", "string", "integer", "floating", "number", "time", "enumeration"
    };
    static constexpr std::array value{Unknown, Any, String, Integer, Floating, Number, Time, Enumeration};
};

template <> struct glz::meta<loglib::LogConfiguration::LogFilter::Type>
{
    using enum loglib::LogConfiguration::LogFilter::Type;
    static constexpr std::array keys{"string", "time", "enumeration"};
    static constexpr std::array value{String, Time, Enumeration};
};

template <> struct glz::meta<loglib::LogConfiguration::LogFilter::Match>
{
    using enum loglib::LogConfiguration::LogFilter::Match;
    static constexpr std::array keys{"exactly", "contains", "regularExpression", "wildcard"};
    static constexpr std::array value{Exactly, Contains, RegularExpression, Wildcard};
};
