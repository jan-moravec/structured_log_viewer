#pragma once

#include "loglib/log_configuration.hpp"

#include <glaze/glaze.hpp>

#include <array>

// Glaze meta specializations for `LogConfiguration::Type` and
// `LogConfiguration::LogFilter::Type` / `Match`. Forces string-based
// JSON encoding so adding new variants does not silently shift wire
// indices, and so saved configs are human-readable.
//
// Include this from every TU that calls `glz::read_json` /
// `glz::write_json` on a `LogConfiguration`. Out-of-line so the heavy
// `<glaze/glaze.hpp>` include does not leak into the public header.
//
// The C++ enumerator and JSON spelling are kept in lockstep
// everywhere. `floating` is used uniformly because `double` is a
// reserved C++ keyword and we do not want a translation table sitting
// between the wire and the source.

template <>
struct glz::meta<loglib::LogConfiguration::Type>
{
    using enum loglib::LogConfiguration::Type;
    static constexpr std::array keys{
        "unknown", "any", "string", "integer", "floating", "number", "time", "enumeration"
    };
    static constexpr std::array value{unknown, any, string, integer, floating, number, time, enumeration};
};

template <>
struct glz::meta<loglib::LogConfiguration::LogFilter::Type>
{
    using enum loglib::LogConfiguration::LogFilter::Type;
    static constexpr std::array keys{"string", "time", "enumeration"};
    static constexpr std::array value{string, time, enumeration};
};

template <>
struct glz::meta<loglib::LogConfiguration::LogFilter::Match>
{
    using enum loglib::LogConfiguration::LogFilter::Match;
    static constexpr std::array keys{"exactly", "contains", "regularExpression", "wildcard"};
    static constexpr std::array value{exactly, contains, regularExpression, wildcard};
};
