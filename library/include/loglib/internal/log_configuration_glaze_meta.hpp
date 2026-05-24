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
        "any", "string", "boolean", "integer", "floating", "number", "time", "enumeration", "level"
    };
    static constexpr std::array value{Any, String, Boolean, Integer, Floating, Number, Time, Enumeration, Level};
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

template <> struct glz::meta<loglib::LogConfiguration::Source::Kind>
{
    using enum loglib::LogConfiguration::Source::Kind;
    static constexpr std::array keys{"file", "networkStream"};
    static constexpr std::array value{File, NetworkStream};
};

// Pinned object schemas for the nested wire types. Glaze's default
// reflection would happily round-trip these without an explicit
// `meta`, but the implicit form makes any field rename a silent
// breaking schema change (old JSON parses with default-constructed
// new field, written JSON loses the old name -- every previously-
// written file becomes unrecoverable). Pinning the field names here
// turns a rename into a compile-time conflict that has to be
// addressed deliberately (e.g. via a migration shim or by
// preserving the old field name explicitly).
//
// The names below are the current implicit-reflection field names;
// switching to explicit `meta` here is a no-op for on-disk JSON.
template <> struct glz::meta<loglib::LogConfiguration::Source>
{
    using T = loglib::LogConfiguration::Source;
    static constexpr auto value =
        object("kind", &T::kind, "locators", &T::locators, "locatorDedupKeys", &T::locatorDedupKeys);
};

template <> struct glz::meta<loglib::LogConfiguration::Column>
{
    using T = loglib::LogConfiguration::Column;
    static constexpr auto value = object(
        "header",
        &T::header,
        "keys",
        &T::keys,
        "printFormat",
        &T::printFormat,
        "type",
        &T::type,
        "parseFormats",
        &T::parseFormats,
        "visible",
        &T::visible,
        "levelMapping",
        &T::levelMapping,
        "autoDetect",
        &T::autoDetect
    );
};

template <> struct glz::meta<loglib::LogConfiguration::LogFilter>
{
    using T = loglib::LogConfiguration::LogFilter;
    static constexpr auto value = object(
        "type",
        &T::type,
        "row",
        &T::row,
        "filterString",
        &T::filterString,
        "matchType",
        &T::matchType,
        "filterBegin",
        &T::filterBegin,
        "filterEnd",
        &T::filterEnd,
        "filterMinValue",
        &T::filterMinValue,
        "filterMaxValue",
        &T::filterMaxValue,
        "filterValues",
        &T::filterValues
    );
};

template <> struct glz::meta<loglib::LogConfiguration::Sort>
{
    using T = loglib::LogConfiguration::Sort;
    static constexpr auto value = object("columnIndex", &T::columnIndex, "descending", &T::descending);
};
// NOLINTEND(readability-identifier-naming)
