/**
 * @brief Built-in and user regex templates for common log formats, and the catalog that holds them.
 *
 * A RegexTemplate pairs a PCRE2 pattern with a name, sample lines and rules for multi-line records. The built-in
 * templates ship inside the library. A RegexTemplateCatalog is an immutable list of templates in probe order: the
 * built-in ones, then the user's. Format detection takes a catalog as an argument, so two parts of a program can detect
 * with different template sets at the same time. The header also loads templates from a folder of JSON files.
 *
 * A catalog is a value. Copies are cheap and share their compiled patterns, and every function on it is safe to call
 * from any thread.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/config/source_descriptor.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
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
 * Throws FormatError if the text is not valid JSON or has a key the template does not define. The message names the key
 * or the position of the problem.
 */
[[nodiscard]] RegexTemplate ParseRegexTemplate(std::string_view content);

/**
 * @brief Writes a template as indented JSON, as saved in a user template file. Throws FormatError if encoding
 * fails.
 */
[[nodiscard]] std::string SerializeRegexTemplate(const RegexTemplate &tmpl);

/**
 * @brief An immutable list of regex templates in the order detection tries them.
 *
 * Built-in templates come first, sorted by priority, and the extra templates follow, also sorted by priority. The two
 * groups are never mixed, so a careless user priority cannot take a match from a shipped template. The compiled patterns
 * that detection needs are built on the first call to Detect and shared by every copy of the catalog.
 *
 * Pointers returned by a catalog stay valid as long as the catalog or any copy of it lives.
 */
class RegexTemplateCatalog
{
public:
    /** @brief The templates that ship with the library. It is built once and never changes. */
    [[nodiscard]] static const RegexTemplateCatalog &Builtin();

    /**
     * @brief Returns a catalog with the built-in templates followed by extras.
     *
     * The extras are sorted by priority, and templates with equal priority keep their order. This catalog is not
     * changed. A catalog made from a catalog that already has extras replaces them.
     */
    [[nodiscard]] RegexTemplateCatalog WithExtras(std::vector<RegexTemplate> extras) const;

    /** @brief Every template in probe order: the built-in ones by priority, then the extras by priority. */
    [[nodiscard]] std::span<const RegexTemplate> Templates() const noexcept;

    /** @brief The built-in templates in probe order. Templates whose embedded JSON cannot be parsed are left out. */
    [[nodiscard]] std::span<const RegexTemplate> Builtins() const noexcept;

    /** @brief The extra templates in probe order. */
    [[nodiscard]] std::span<const RegexTemplate> Extras() const noexcept;

    /**
     * @brief The slug of each built-in template, in the same order as Builtins.
     *
     * The slug is the name of the template's data file without the extension, for example apache_combined. It stays the
     * same when the display name changes, and slp formats prints it.
     */
    [[nodiscard]] std::span<const std::string> BuiltinSlugs() const noexcept;

    /** @brief Finds the template whose pattern is exactly pattern, or null. Use it to name a saved pattern. */
    [[nodiscard]] const RegexTemplate *FindByPattern(std::string_view pattern) const noexcept;

    /** @brief Finds a built-in template by its slug, or null. */
    [[nodiscard]] const RegexTemplate *FindBuiltinBySlug(std::string_view slug) const noexcept;

    /**
     * @brief Returns the first template with auto-detection turned on that matches enough of the sample, or null.
     *
     * The first non-blank lines of the sample must match the whole pattern. At least two lines are needed, so a single
     * line cannot claim a file. At most PROBE_BYTES_BUDGET bytes of the sample are read.
     */
    [[nodiscard]] const RegexTemplate *Detect(std::string_view sample) const;

private:
    struct Data;
    explicit RegexTemplateCatalog(std::shared_ptr<const Data> data) noexcept;

    std::shared_ptr<const Data> mData;
};

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
