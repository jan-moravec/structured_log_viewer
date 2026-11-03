/**
 * @brief Description of where a log came from.
 *
 * A Source records the kind of source, such as a file or a network stream, its format, and the locations to reopen it
 * from. Each location keeps the path shown to the user and a normalized key used to tell whether two paths are the same
 * file.
 *
 * This is part of the schema module.
 */
#pragma once

#include <algorithm>
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
 * fails the columns and filters still apply. A saved source field that is not recognized drops the source and keeps the
 * rest, because unknown fields are ignored.
 *
 * Locators hold each path together with its comparison key, so the two cannot get out of step. The saved file keeps
 * them as two parallel lists, locators and locatorDedupKeys, which ReplaceDisplayPaths and ReplaceDedupKeys fill.
 */
struct Source
{
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

    /**
     * @brief The parser a source was opened with.
     *
     * It is saved because a network stream has nothing to look at when it is restored, and a live session chooses its
     * parser before the first byte arrives. A new Source uses Json. In the saved file it is a string such as json,
     * logfmt or csv. New values go at the end and the order never changes.
     */
    enum class Format
    {
        Json,
        Logfmt,
        Csv,
        /** @brief The regex parser. It needs regexPattern and refuses to run without one. */
        Regex,
    };

    Kind kind = Kind::File;
    Format format = Format::Json;
    std::vector<SourceLocator> locators;
    /**
     * @brief The PCRE2 pattern, with named groups written as (?<Name>...), for the Regex format. Empty for other
     * formats.
     *
     * It is saved so a reopened session or stream parses with the same template. The name of the matching template, if
     * there is one, is looked up for display with FindTemplateByPattern.
     */
    std::string regexPattern;

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
[[nodiscard]] inline bool HasLocators(const std::optional<Source> &source) noexcept
{
    return source.has_value() && !source->locators.empty();
}

/**
 * @brief Adds a locator to a source. Callers pass the comparison key, because the library does not know how to
 * normalize paths in the way the application does.
 */
inline void AppendLocator(Source &target, std::string displayPath, std::string dedupKey)
{
    target.locators.emplace_back(std::move(displayPath), std::move(dedupKey));
}

/** @brief Removes all locators. */
inline void ClearLocators(Source &target)
{
    target.locators.clear();
    target.pendingDedupKeys.clear();
}

/** @brief True if the key is the comparison key of any locator. */
[[nodiscard]] inline bool ContainsDedupKey(const Source &source, std::string_view key) noexcept
{
    return std::ranges::any_of(source.locators, [key](const SourceLocator &locator) {
        return locator.dedupKey == key;
    });
}

} // namespace loglib
