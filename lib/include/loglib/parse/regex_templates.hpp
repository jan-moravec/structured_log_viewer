/**
 * @brief Built-in and user regex templates for common log formats.
 *
 * A RegexTemplate pairs a PCRE2 pattern with a name, sample lines and rules for multi-line records. The built-in
 * templates ship inside the library. Users can add their own, which the library keeps in a registry that format
 * detection reads. The header also loads templates from a folder of JSON files and looks them up by name or by pattern.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/schema/source_descriptor.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{

/**
 * @brief The priority a user template gets when it does not set one.
 *
 * The built-in templates use priorities from 10 to 30, so a user template always probes after them and leaves room on
 * both sides for users who want to order their own. The template file schema and the editor both use this value.
 */
inline constexpr int USER_TEMPLATE_DEFAULT_PRIORITY = 100;

/** @brief The name used for a mode in template JSON files. */
[[nodiscard]] std::string_view ContinuationModeName(ContinuationMode mode) noexcept;

/** @brief Reads a mode from its JSON name. Returns nullopt for an unknown name. */
[[nodiscard]] std::optional<ContinuationMode> ParseContinuationMode(std::string_view name) noexcept;

/**
 * @brief A named PCRE2 pattern that splits one log line into columns.
 *
 * The pattern uses PCRE2 syntax with named groups written as (?<Name>...). Each named group becomes a column whose key
 * is the group name. Anonymous groups are ignored.
 *
 * The sample lines serve two purposes. Format detection matches the pattern against the first lines of a file, and the
 * regex parser tests check that every shipped template parses the lines it advertises.
 *
 * Built-in templates are JSON files, one per template, under lib/data/regex_templates and embedded at build time.
 * User templates are JSON files in the regex_templates folder of the application data directory. They need no build.
 */
struct RegexTemplate
{
    /**
     * @brief The name to show when listing templates. Names must be unique: a user template with the name of a built-in
     * one replaces it.
     */
    std::string name;

    /**
     * @brief The PCRE2 pattern. It is compiled once and shared by all parser threads, and each thread keeps its own
     * match data.
     */
    std::string pattern;

    /**
     * @brief Lines the pattern is known to match. Detection uses them when autoDetect is true, and tests use them as
     * fixtures.
     */
    std::vector<std::string> sampleLines;

    /**
     * @brief Whether detection tries this template on files it does not recognize.
     *
     * A template with this set to false is still offered for manual selection. It is true when a file does not say.
     */
    bool autoDetect = true;

    /**
     * @brief The order in which detection tries templates. Lower values go first, and equal values keep their file
     * order.
     *
     * The built-in priorities put specific patterns before loose ones, so Combined Log Format is tried before Common
     * Log Format. A file that does not set one gets USER_TEMPLATE_DEFAULT_PRIORITY.
     */
    int priority = USER_TEMPLATE_DEFAULT_PRIORITY;

    /**
     * @brief A free-text description of the format: what writes it, what the columns mean, and edge cases.
     *
     * Templates ported from other projects also credit the source and its license here, and the third-party license
     * file is checked against these credits. It may be empty.
     */
    std::string description;

    /** @brief How continuation lines are recognized. A template file that does not set it gets None. */
    ContinuationMode continuationMode = ContinuationMode::None;

    /**
     * @brief The pattern that recognizes a record's first line when continuationMode is UntilNextHeader. If empty,
     * pattern is used. Other modes ignore it.
     */
    std::string headerAnchor;
};

/**
 * @brief Reads one template from its JSON text. Fields the text leaves out keep their default.
 *
 * Throws std::runtime_error if the text is invalid. The message includes the position of the problem.
 */
[[nodiscard]] RegexTemplate ParseRegexTemplate(std::string_view content);

/**
 * @brief Writes a template as indented JSON, as saved in a user template file. Throws std::runtime_error if encoding
 * fails.
 */
[[nodiscard]] std::string SerializeRegexTemplate(const RegexTemplate &tmpl);

/**
 * @brief The templates that ship with the library, in the order detection tries them.
 *
 * They are read from the embedded JSON files on the first call and never change afterwards, so the returned span stays
 * valid for the whole process. The description of a ported template names its source.
 */
[[nodiscard]] std::span<const RegexTemplate> BuiltinRegexTemplates() noexcept;

/**
 * @brief A built-in template together with its slug.
 *
 * The slug is the name of the template's data file without the extension, for example apache_combined. It stays the
 * same when the display name changes, and slp formats prints it.
 */
struct BuiltinRegexTemplateInfo
{
    std::string slug;
    RegexTemplate tmpl;
};

/**
 * @brief Returns a copy of every built-in template with its slug.
 *
 * Templates whose embedded JSON cannot be parsed are left out. The order is the same as BuiltinRegexTemplates.
 */
[[nodiscard]] std::vector<BuiltinRegexTemplateInfo> BuiltinRegexTemplateCatalog();

/**
 * @brief Registers the user's templates so format detection tries them as well as the built-in ones.
 *
 * Each call replaces the earlier set, and an empty span clears it. The templates are copied, so the caller's storage
 * may go away. It is safe to call repeatedly, for example when templates are reloaded from disk; the change applies
 * from the next probe.
 *
 * Built-in templates are always tried before user templates, whatever their priority, so a user template cannot take
 * over a match from a shipped one. Within each group the templates are ordered by priority.
 */
void SetExtraRegexTemplates(std::span<const RegexTemplate> extras);

/**
 * @brief Finds the built-in or registered template whose pattern is exactly pattern.
 *
 * Use it to show the template name for a saved pattern. Returns nullopt if the pattern is custom and unsaved. The
 * template is returned by value, so a later SetExtraRegexTemplates call cannot invalidate it.
 */
[[nodiscard]] std::optional<RegexTemplate> FindTemplateByPattern(std::string_view pattern);

/**
 * @brief The result of loading a folder of templates.
 *
 * Files are read in file name order. A file that cannot be read or parsed is skipped, and a message about it is added
 * to errors.
 */
struct RegexTemplateLoadResult
{
    std::vector<RegexTemplate> templates;
    std::vector<std::string> errors;
};

/**
 * @brief Loads every JSON template file in a folder.
 *
 * A folder that does not exist gives an empty result and is not created. Each skipped file adds one entry to errors.
 */
[[nodiscard]] RegexTemplateLoadResult LoadRegexTemplatesFromDirectory(const std::filesystem::path &directory);

} // namespace loglib
