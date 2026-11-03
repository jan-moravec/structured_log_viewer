/**
 * @brief Access to the regex templates compiled into the library.
 *
 * The build turns the JSON files under data/regex_templates into a table of strings. This header exposes that table to
 * the regex_templates code.
 *
 * This is part of the parse module.
 */
#pragma once

#include <span>
#include <string_view>

namespace loglib::internal
{

/**
 * @brief One built-in regex template that is compiled into the library.
 *
 * The source is a short name, the file name of the template, used only in messages if the embedded text cannot be
 * parsed. The json is the content of the file as it is.
 *
 * The data comes from a source file that CMake generates from lib/data/regex_templates/*.json when it configures the
 * build, with one entry per file ordered by file name. Adding or removing a built-in template therefore only touches
 * that folder.
 */
struct EmbeddedRegexTemplate
{
    std::string_view source;
    std::string_view json;
};

/** @brief Returns all embedded templates. The function is defined in the generated source file. */
[[nodiscard]] std::span<const EmbeddedRegexTemplate> EmbeddedBuiltinRegexTemplates() noexcept;

} // namespace loglib::internal
