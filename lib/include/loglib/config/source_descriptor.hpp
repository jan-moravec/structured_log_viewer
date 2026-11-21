/**
 * @brief Description of where a log came from.
 *
 * A SourceDescriptor records the kind of source, such as a file or a network stream, its format and settings, and the
 * locations to reopen it from. Each location keeps the path shown to the user and a normalized key used to tell whether
 * two paths are the same file.
 *
 * This is part of the config module.
 */
#pragma once

#include "loglib/config/format_spec.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{

/**
 * @brief One saved source path, with the form to show and a normalized form to compare.
 *
 * displayPath keeps the original spelling and is the path to open. dedupKey is the form used to tell whether two paths
 * are the same file; on Windows it is lower-cased. An empty dedupKey after loading means the saved file did not have
 * one, and the application fills it in with BackfillLocatorDedupKeys.
 */
struct SourceLocator
{
    std::string displayPath;
    std::string dedupKey;

    SourceLocator() = default;

    /** @brief Builds a locator from a string literal, so that braces such as .locators = {"path"} compile. */
    // NOLINTNEXTLINE(cppcoreguidelines-explicit-constructor,google-explicit-constructor,hicpp-explicit-conversions,misc-explicit-constructor)
    SourceLocator(const char *display)
        : displayPath(display != nullptr ? display : "")
    {
    }

    // NOLINTNEXTLINE(cppcoreguidelines-explicit-constructor,google-explicit-constructor,hicpp-explicit-conversions,misc-explicit-constructor)
    SourceLocator(std::string display, std::string dedup = {})
        : displayPath(std::move(display)), dedupKey(std::move(dedup))
    {
    }

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
 * them as two parallel lists, locators and locatorDedupKeys, which ReplaceDisplayPaths and ReplaceDedupKeys fill.
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

    /**
     * @brief Holds locatorDedupKeys that arrive before the paths while a file is loaded. It is not saved.
     * ReplaceDisplayPaths uses and clears it. Keys that never get a list of paths do not become locators.
     */
    std::vector<std::string> pendingDedupKeys;

    /**
     * @brief Replaces the paths and keeps the comparison keys that still fit.
     *
     * The number of paths decides the number of locators. Loading calls this and ReplaceDedupKeys in either order. Keys
     * held in pendingDedupKeys are applied by position and then discarded.
     */
    void ReplaceDisplayPaths(std::vector<std::string> paths);

    /**
     * @brief Sets the comparison keys by position.
     *
     * While there are no locators yet, the keys wait in pendingDedupKeys until ReplaceDisplayPaths sets how many there
     * are. Once paths exist, keys beyond their number are ignored.
     */
    void ReplaceDedupKeys(std::vector<std::string> keys);

    /** @brief The paths in locator order, for writing the saved file. */
    [[nodiscard]] std::vector<std::string> DisplayPaths() const;

    /** @brief The comparison keys in locator order, for writing the saved file. */
    [[nodiscard]] std::vector<std::string> DedupKeys() const;
};

/** @brief True if there is a source with at least one locator, so that it can be opened. */
[[nodiscard]] inline bool HasLocators(const std::optional<SourceDescriptor> &source) noexcept
{
    return source.has_value() && !source->locators.empty();
}

/**
 * @brief Adds a locator to a source. Callers pass the comparison key, because the library does not know how to
 * normalize paths in the way the application does.
 */
inline void AppendLocator(SourceDescriptor &target, std::string displayPath, std::string dedupKey)
{
    target.locators.emplace_back(std::move(displayPath), std::move(dedupKey));
}

/** @brief Removes all locators. */
inline void ClearLocators(SourceDescriptor &target)
{
    target.locators.clear();
    target.pendingDedupKeys.clear();
}

/** @brief True if the key is the comparison key of any locator. */
[[nodiscard]] inline bool ContainsDedupKey(const SourceDescriptor &source, std::string_view key) noexcept
{
    return std::ranges::any_of(source.locators, [key](const SourceLocator &locator) {
        return locator.dedupKey == key;
    });
}

} // namespace loglib
