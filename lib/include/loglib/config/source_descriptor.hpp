/**
 * @brief Description of where a log came from.
 *
 * A SourceDescriptor records the kind of source, such as a file or a network stream, its format and settings, and the
 * locations to reopen it from. Each location keeps the path to open and a normalized key used to tell whether two paths
 * are the same file.
 *
 * This is part of the config module.
 */
#pragma once

#include "loglib/config/format_spec.hpp"
#include "loglib/core/path_key.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

/**
 * @brief One saved source path, with the form to open and a normalized form to compare.
 *
 * path keeps the original spelling and is the path to open. key is the form that tells whether two paths are the same
 * file, made by MakePathKey for a file, and anchors refer to a source by it. A network source has no file, so its path
 * and its key are both the address. The saved file must hold a key for every locator.
 */
struct SourceLocator
{
    std::string path;
    std::string key;

    friend bool operator==(const SourceLocator &, const SourceLocator &) = default;
};

/**
 * @brief Where a log came from, as saved in a configuration.
 *
 * An empty source in LogConfiguration means none is bound. The application may reopen the source on load, and if that
 * fails the columns and filters still apply. An unknown field in a saved source is an error, as for any other part of
 * a configuration.
 *
 * Locators hold each path together with its comparison key, so the two cannot get out of step. The saved file keeps
 * them as a list of objects with the keys path and key.
 */
struct SourceDescriptor
{
    /** @brief What the source reads from. */
    enum class Kind
    {
        File,
        NetworkStream,
        /**
         * @brief Standard input from a pipe. It is used once per session and is not reopened automatically, so
         * applications do not save such a session. Its locator is the name <stdin>.
         */
        Stdin,
    };

    Kind kind = Kind::File;
    /**
     * @brief The parser the source was opened with, and its settings.
     *
     * It is saved because a network stream has nothing to look at when it is restored, and a live session chooses its
     * parser before the first byte arrives. The regex settings are saved with the pattern, so a restored session parses
     * multi-line records as it did, whether or not a template for the pattern still exists. The name of the matching
     * template, if there is one, is looked up for display with RegexTemplateCatalog::FindByPattern.
     */
    FormatSpec format;
    std::vector<SourceLocator> locators;
    /**
     * @brief Whether files added later also bring in their rotated companions. A restore opens the saved locators as
     * they are and does not search again. Configurations saved without this value get true.
     */
    bool followRotationSiblings = true;
};

/** @brief True if there is a source with at least one locator, so that it can be opened. */
[[nodiscard]] inline bool HasLocators(const std::optional<SourceDescriptor> &source) noexcept
{
    return source.has_value() && !source->locators.empty();
}

/** @brief Adds a file to a source, with the key that MakePathKey gives for it. */
inline void AppendLocator(SourceDescriptor &target, std::string path)
{
    std::string key = MakePathKey(path);
    target.locators.push_back({.path = std::move(path), .key = std::move(key)});
}

/** @brief Removes all locators. */
inline void ClearLocators(SourceDescriptor &target)
{
    target.locators.clear();
}

/** @brief True if the key is the key of any locator. */
[[nodiscard]] inline bool ContainsPathKey(const SourceDescriptor &source, std::string_view key) noexcept
{
    return std::ranges::any_of(source.locators, [key](const SourceLocator &locator) { return locator.key == key; });
}

} // namespace loglib
