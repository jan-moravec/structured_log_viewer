#pragma once

#include "loglib/filter_expression.hpp"
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

template <> struct glz::meta<loglib::LeafRule::Type>
{
    using enum loglib::LeafRule::Type;
    static constexpr std::array keys{"string", "time", "enumeration", "number", "boolean"};
    static constexpr std::array value{String, Time, Enumeration, Number, Boolean};
};

template <> struct glz::meta<loglib::LeafRule::Match>
{
    using enum loglib::LeafRule::Match;
    static constexpr std::array keys{"exactly", "contains", "regularExpression", "wildcard"};
    static constexpr std::array value{Exactly, Contains, RegularExpression, Wildcard};
};

// `HighlightRule::Type` / `HighlightRule::Match` are aliases of
// `LeafRule::Type` / `LeafRule::Match`, so their meta comes from
// the specialisations above.

template <> struct glz::meta<loglib::LogConfiguration::Source::Kind>
{
    using enum loglib::LogConfiguration::Source::Kind;
    static constexpr std::array keys{"file", "networkStream"};
    static constexpr std::array value{File, NetworkStream};
};

template <> struct glz::meta<loglib::LogConfiguration::Source::Format>
{
    using enum loglib::LogConfiguration::Source::Format;
    static constexpr std::array keys{"json", "logfmt", "csv", "regex"};
    static constexpr std::array value{Json, Logfmt, Csv, Regex};
};

// Pinned wire schemas for nested types. Explicit names turn a field
// rename into a compile-time conflict instead of a silent breaking
// schema change. The names match the current implicit reflection,
// so adopting these meta declarations is a no-op for on-disk JSON.
template <> struct glz::meta<loglib::LogConfiguration::Source>
{
    using T = loglib::LogConfiguration::Source;
    static constexpr auto value = object(
        "kind",
        &T::kind,
        "format",
        &T::format,
        "locators",
        &T::locators,
        "locatorDedupKeys",
        &T::locatorDedupKeys,
        "regexPattern",
        &T::regexPattern
    );
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

// A leaf of the filter tree. Column identity is a subset-match
// against `LogConfiguration::Column::keys`, so leaves survive
// `MoveColumn` and cross-source apply without any remap.
template <> struct glz::meta<loglib::LeafRule>
{
    using T = loglib::LeafRule;
    static constexpr auto value = object(
        "type",
        &T::type,
        "columnKeys",
        &T::columnKeys,
        "matchType",
        &T::matchType,
        "filterString",
        &T::filterString,
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

// Wire-format shape for the tagged boolean expression tree.
//
// The `variant<Leaf, And, Or, Not>` serialises as an object whose
// key is the discriminator (`"leaf"` / `"and"` / `"or"` / `"not"`)
// and whose value is the corresponding node payload. Glaze picks
// this shape when the variant has meta specialisations for both
// the variant itself and each alternative.
template <> struct glz::meta<loglib::FilterExpression::Leaf>
{
    using T = loglib::FilterExpression::Leaf;
    static constexpr auto value = object("rule", &T::rule);
};

template <> struct glz::meta<loglib::FilterExpression::And>
{
    using T = loglib::FilterExpression::And;
    static constexpr auto value = object("children", &T::children);
};

template <> struct glz::meta<loglib::FilterExpression::Or>
{
    using T = loglib::FilterExpression::Or;
    static constexpr auto value = object("children", &T::children);
};

// `Not::child` is a `unique_ptr<FilterExpression>`; Glaze handles
// smart pointers automatically. A null child on-disk is represented
// as `null` and reconstructed as a null pointer -- editors treat
// that as an unfinished tree and coerce it to match-all on load.
template <> struct glz::meta<loglib::FilterExpression::Not>
{
    using T = loglib::FilterExpression::Not;
    static constexpr auto value = object("child", &T::child);
};

template <> struct glz::meta<loglib::FilterExpression>
{
    using T = loglib::FilterExpression;
    static constexpr auto value = object("node", &T::node);
};

template <> struct glz::meta<loglib::LogConfiguration::Sort>
{
    using T = loglib::LogConfiguration::Sort;
    static constexpr auto value = object("columnIndex", &T::columnIndex, "descending", &T::descending);
};

// Wire schema for one anchor. On-disk JSON:
//   { "locator": "...",    // stable per-source id (file path, stream)
//     "lineId":  1234,     // provider-assigned line id
//     "colorIndex": 0,     // palette slot
//     "note":    "..." }   // optional; sanitised + byte-capped by
//                          // `AnchorManager::SanitiseNote`. Absent
//                          // on pre-notes configs; loads as "".
//
// Add new fields at the end of `object(...)` with defaults in
// `AnchorEntry` so old configs still round-trip.
// `error_on_unknown_keys=false` lets old builds tolerate new keys.
template <> struct glz::meta<loglib::LogConfiguration::AnchorEntry>
{
    using T = loglib::LogConfiguration::AnchorEntry;
    static constexpr auto value =
        object("locator", &T::locator, "lineId", &T::lineId, "colorIndex", &T::colorIndex, "note", &T::note);
};

template <> struct glz::meta<loglib::LogConfiguration::HighlightRule>
{
    using T = loglib::LogConfiguration::HighlightRule;
    static constexpr auto value = object(
        "name",
        &T::name,
        "enabled",
        &T::enabled,
        "columnKeys",
        &T::columnKeys,
        "type",
        &T::type,
        "matchType",
        &T::matchType,
        "filterString",
        &T::filterString,
        "filterBegin",
        &T::filterBegin,
        "filterEnd",
        &T::filterEnd,
        "filterMinValue",
        &T::filterMinValue,
        "filterMaxValue",
        &T::filterMaxValue,
        "filterValues",
        &T::filterValues,
        "foregroundIndex",
        &T::foregroundIndex,
        "backgroundIndex",
        &T::backgroundIndex,
        "bold",
        &T::bold,
        "italic",
        &T::italic
    );
};
// NOLINTEND(readability-identifier-naming)
