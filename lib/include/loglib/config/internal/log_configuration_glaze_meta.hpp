/**
 * @brief JSON field names, enum spellings and the document shape of a configuration file.
 *
 * This header tells the JSON library how to read and write each schema type, and defines the document that a
 * configuration file and the metadata line of a session bundle hold. It is only included where the configuration is
 * serialized, so the public headers stay free of the JSON library.
 *
 * This is part of the schema module.
 */
#pragma once

#include "loglib/config/filter_expression.hpp"
#include "loglib/config/log_configuration.hpp"

#include <fmt/format.h>
#include <glaze/glaze.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib::internal
{

/** @brief The version that a configuration file must carry. */
inline constexpr std::uint32_t CONFIGURATION_FORMAT_VERSION = 1;

/**
 * @brief A column as a file holds it. The type is present only when the user chose it, which is when the column has
 * autoDetect off. The other defaults are those of Column.
 */
struct ColumnDocument
{
    std::string header;
    std::vector<std::string> keys;
    std::optional<ColumnType> type;
    std::string printFormat;
    std::vector<std::string> parseFormats;
    bool visible = true;
    std::vector<std::pair<std::string, std::string>> levelMapping;
};

/** @brief The JSON document of a configuration. A missing view and a missing source have their defaults. */
struct ConfigurationDocument
{
    std::uint32_t version = 0;
    std::vector<ColumnDocument> columns;
    std::optional<SessionView> view;
    std::optional<Source> source;
};

/** @brief The document to write for a configuration. A Columns scope leaves out the view and the source. */
[[nodiscard]] inline ConfigurationDocument MakeDocument(const LogConfiguration &configuration, SaveScope scope)
{
    ConfigurationDocument document;
    document.version = CONFIGURATION_FORMAT_VERSION;
    document.columns.reserve(configuration.columns.size());
    for (const Column &column : configuration.columns)
    {
        ColumnDocument &out = document.columns.emplace_back();
        out.header = column.header;
        out.keys = column.keys;
        if (!column.autoDetect)
        {
            out.type = column.type;
        }
        out.printFormat = column.printFormat;
        out.parseFormats = column.parseFormats;
        out.visible = column.visible;
        out.levelMapping = column.levelMapping;
    }
    if (scope == SaveScope::Full)
    {
        document.view = configuration.view;
        document.source = configuration.source;
    }
    return document;
}

/** @brief The configuration that a document describes. The caller has checked the version. */
[[nodiscard]] inline LogConfiguration MakeConfiguration(ConfigurationDocument document)
{
    LogConfiguration configuration;
    configuration.columns.reserve(document.columns.size());
    for (ColumnDocument &in : document.columns)
    {
        Column &column = configuration.columns.emplace_back();
        column.header = std::move(in.header);
        column.keys = std::move(in.keys);
        column.autoDetect = !in.type.has_value();
        column.type = in.type.value_or(ColumnType::Any);
        column.printFormat = std::move(in.printFormat);
        column.parseFormats = std::move(in.parseFormats);
        column.visible = in.visible;
        column.levelMapping = std::move(in.levelMapping);
    }
    if (document.view.has_value())
    {
        configuration.view = std::move(*document.view);
    }
    configuration.source = std::move(document.source);
    return configuration;
}

/**
 * @brief Describes a read error of a configuration document. For an unknown or missing key the first line names the
 * key, because callers often show only that line. The lines after it point at the place in the content.
 */
[[nodiscard]] inline std::string DescribeConfigurationError(const glz::error_ctx &error, std::string_view content)
{
    if (error.ec == glz::error_code::unknown_key)
    {
        std::string key;
        if (error.count < content.size())
        {
            const size_t end = content.find('"', error.count);
            key =
                content.substr(error.count, end == std::string_view::npos ? std::string_view::npos : end - error.count);
        }
        return fmt::format("unknown key '{}'.\n{}", key, glz::format_error(error, content));
    }
    if (error.ec == glz::error_code::missing_key)
    {
        return fmt::format(
            "the required key '{}' is missing.\n{}", error.custom_error_message, glz::format_error(error, content)
        );
    }
    return glz::format_error(error, content);
}

/** @brief A base for the metas of types whose keys may all be missing. They then keep the default of their member. */
struct AllKeysOptional
{
    static constexpr bool
    requires_key(std::string_view /*key*/, bool /*nullable*/) // NOLINT(readability-identifier-naming)
    {
        return false;
    }
};

} // namespace loglib::internal

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

template <> struct glz::meta<loglib::ContinuationMode>
{
    using enum loglib::ContinuationMode;
    // The names pair with the values by position. Their case is part of the saved format.
    static constexpr std::array keys{"none", "indented", "untilNextHeader"};
    static constexpr std::array value{None, Indented, UntilNextHeader};
};

// The saved field names of the nested types are listed explicitly. Renaming a member then fails to compile and cannot
// silently change the saved format.
template <> struct glz::meta<loglib::Source> : loglib::internal::AllKeysOptional
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
        "regexContinuationMode",
        &T::regexContinuationMode,
        "regexHeaderAnchor",
        &T::regexHeaderAnchor,
        "followRotationSiblings",
        &T::followRotationSiblings
    );
};

// A column needs a header and keys. Every other field has the default of Column.
template <> struct glz::meta<loglib::internal::ColumnDocument>
{
    using T = loglib::internal::ColumnDocument;
    static constexpr bool requires_key(std::string_view key, bool /*nullable*/)
    {
        return key == "header" || key == "keys";
    }
    static constexpr auto value = object(
        "header",
        &T::header,
        "keys",
        &T::keys,
        "type",
        &T::type,
        "printFormat",
        &T::printFormat,
        "parseFormats",
        &T::parseFormats,
        "visible",
        &T::visible,
        "levelMapping",
        &T::levelMapping
    );
};

// The version and the columns are required. The view and the source may be missing.
template <> struct glz::meta<loglib::internal::ConfigurationDocument>
{
    using T = loglib::internal::ConfigurationDocument;
    static constexpr bool requires_key(std::string_view key, bool /*nullable*/)
    {
        return key == "version" || key == "columns";
    }
    static constexpr auto value =
        object("version", &T::version, "columns", &T::columns, "view", &T::view, "source", &T::source);
};

template <> struct glz::meta<loglib::SessionView> : loglib::internal::AllKeysOptional
{
    using T = loglib::SessionView;
    static constexpr auto value = object(
        "expression", &T::expression, "sort", &T::sort, "highlightRules", &T::highlightRules, "anchors", &T::anchors
    );
};

// A leaf of the filter tree. It finds its column by matching its keys against Column::keys.
template <> struct glz::meta<loglib::LeafRule> : loglib::internal::AllKeysOptional
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
template <> struct glz::meta<loglib::FilterExpression::Leaf> : loglib::internal::AllKeysOptional
{
    using T = loglib::FilterExpression::Leaf;
    static constexpr auto value = object("rule", &T::rule);
};

template <> struct glz::meta<loglib::FilterExpression::And> : loglib::internal::AllKeysOptional
{
    using T = loglib::FilterExpression::And;
    static constexpr auto value = object("children", &T::children);
};

template <> struct glz::meta<loglib::FilterExpression::Or> : loglib::internal::AllKeysOptional
{
    using T = loglib::FilterExpression::Or;
    static constexpr auto value = object("children", &T::children);
};

// The child of a Not is a smart pointer, which the JSON library reads and writes by itself. A null child is saved as
// null and loaded as null, and an editor treats such a tree as unfinished and turns it into match-all.
template <> struct glz::meta<loglib::FilterExpression::Not> : loglib::internal::AllKeysOptional
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

template <> struct glz::meta<loglib::Sort> : loglib::internal::AllKeysOptional
{
    using T = loglib::Sort;
    static constexpr auto value = object("columnKeys", &T::columnKeys, "descending", &T::descending);
};

// The saved form of one anchor:
//   { "locator": "...",    // identifies the source
//     "lineId":  1234,     // the line id from the parser
//     "colorIndex": 0,     // the palette color
//     "note":    "..." }   // optional, loads as an empty string when missing
// Every field is optional and has the default of AnchorEntry.
template <> struct glz::meta<loglib::AnchorEntry> : loglib::internal::AllKeysOptional
{
    using T = loglib::AnchorEntry;
    static constexpr auto value =
        object("locator", &T::locator, "lineId", &T::lineId, "colorIndex", &T::colorIndex, "note", &T::note);
};

template <> struct glz::meta<loglib::HighlightRule> : loglib::internal::AllKeysOptional
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
