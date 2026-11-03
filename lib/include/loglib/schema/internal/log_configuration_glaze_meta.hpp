/**
 * @brief JSON field names and enum spellings for the configuration types.
 *
 * This header tells the JSON library how to read and write each schema type. It is only included where the
 * configuration is serialized, so the public headers stay free of the JSON library.
 *
 * This is part of the schema module.
 */
#pragma once

#include "loglib/schema/filter_expression.hpp"
#include "loglib/schema/log_configuration.hpp"

#include <glaze/glaze.hpp>

#include <array>

// Enums are written as strings, which keeps saved files readable and stable. The strings are lowerCamelCase while the
// C++ enumerators are UpperCamelCase. The specializations live in this header so the public headers do not include the
// JSON library. The name floating is used for the type because double is a keyword.
//
// The names keys and value are required by the JSON library's reflection, so the naming check is turned off here.
// NOLINTBEGIN(readability-identifier-naming)

template <> struct glz::meta<loglib::ColumnType>
{
    using enum loglib::ColumnType;
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

// The types of HighlightRule are aliases of the LeafRule ones, so they use the specializations above.

template <> struct glz::meta<loglib::Source::Kind>
{
    using enum loglib::Source::Kind;
    static constexpr std::array keys{"file", "networkStream", "stdin"};
    static constexpr std::array value{File, NetworkStream, Stdin};
};

template <> struct glz::meta<loglib::Source::Format>
{
    using enum loglib::Source::Format;
    static constexpr std::array keys{"json", "logfmt", "csv", "regex"};
    static constexpr std::array value{Json, Logfmt, Csv, Regex};
};

// The saved field names of the nested types are listed explicitly. Renaming a member then fails to compile and cannot
// silently change the saved format.
template <> struct glz::meta<loglib::Source>
{
    using T = loglib::Source;

    static constexpr auto readDisplayPaths = [](T &self, std::vector<std::string> paths) {
        self.ReplaceDisplayPaths(std::move(paths));
    };
    static constexpr auto writeDisplayPaths = [](const T &self) { return self.DisplayPaths(); };
    static constexpr auto readDedupKeys = [](T &self, std::vector<std::string> keys) {
        self.ReplaceDedupKeys(std::move(keys));
    };
    static constexpr auto writeDedupKeys = [](const T &self) { return self.DedupKeys(); };

    static constexpr auto value = object(
        "kind",
        &T::kind,
        "format",
        &T::format,
        "locators",
        glz::custom<readDisplayPaths, writeDisplayPaths>,
        "locatorDedupKeys",
        glz::custom<readDedupKeys, writeDedupKeys>,
        "regexPattern",
        &T::regexPattern,
        "followRotationSiblings",
        &T::followRotationSiblings
    );
};

template <> struct glz::meta<loglib::Column>
{
    using T = loglib::Column;
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

// A leaf of the filter tree. It finds its column by matching its keys against Column::keys.
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

// The saved form of the filter tree. Each node is an object with a kind field next to its payload:
//   { "kind": "and", "children": [ ... ] }
//   { "kind": "or",  "children": [ ... ] }
//   { "kind": "not", "child": <sub-expression> }
//   { "kind": "leaf", "rule": <LeafRule> }
// The kind comes from the tag of the variant specialization below, and each node type exposes only its payload.
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

// The child of a Not is a smart pointer, which the JSON library reads and writes by itself. A null child is saved as
// null and loaded as null, and an editor treats such a tree as unfinished and turns it into match-all.
template <> struct glz::meta<loglib::FilterExpression::Not>
{
    using T = loglib::FilterExpression::Not;
    static constexpr auto value = object("child", &T::child);
};

// The kind field sits in the same object as the payload fields. The kind names are part of the saved format, so add new
// alternatives at the end and never reorder them.
template <>
struct glz::meta<std::variant<
    loglib::FilterExpression::Leaf,
    loglib::FilterExpression::And,
    loglib::FilterExpression::Or,
    loglib::FilterExpression::Not>>
{
    static constexpr std::string_view tag = "kind";
    static constexpr auto ids = std::array{"leaf", "and", "or", "not"};
};

template <> struct glz::meta<loglib::FilterExpression>
{
    using T = loglib::FilterExpression;
    static constexpr auto value = &T::node;
};

template <> struct glz::meta<loglib::Sort>
{
    using T = loglib::Sort;
    static constexpr auto value = object("columnIndex", &T::columnIndex, "descending", &T::descending);
};

// The saved form of one anchor:
//   { "locator": "...",    // identifies the source
//     "lineId":  1234,     // the line id from the parser
//     "colorIndex": 0,     // the palette color
//     "note":    "..." }   // optional, loads as an empty string when missing
// Add new fields at the end of the object, with defaults in AnchorEntry, so older files still load. Unknown fields are
// ignored, so older builds can read files that have newer fields.
template <> struct glz::meta<loglib::AnchorEntry>
{
    using T = loglib::AnchorEntry;
    static constexpr auto value =
        object("locator", &T::locator, "lineId", &T::lineId, "colorIndex", &T::colorIndex, "note", &T::note);
};

template <> struct glz::meta<loglib::HighlightRule>
{
    using T = loglib::HighlightRule;
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
