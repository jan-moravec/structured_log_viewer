#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{

/**
 * @brief One persisted source path: the display form the user sees and
 * the normalised key used for equality.
 *
 * `displayPath` keeps original case and is what `QFile::open` consumes.
 * `dedupKey` is the byte-equality form (lower-cased on Windows). Empty
 * `dedupKey` after load means the JSON predated the split; the
 * application backfills it via `BackfillLocatorDedupKeys`.
 */
struct SourceLocator
{
    std::string displayPath;
    std::string dedupKey;

    SourceLocator() = default;

    /**
     * @brief Converts a string literal so `.locators = {"path"}` compiles.
     *
     * `initializer_list` copy-initialization cannot chain
     * `const char*` → `std::string` → `SourceLocator`.
     */
    SourceLocator(const char *display)
        : displayPath(display != nullptr ? display : "")
    {
    }

    SourceLocator(std::string display, std::string dedup = {})
        : displayPath(std::move(display)), dedupKey(std::move(dedup))
    {
    }

    friend bool operator==(const SourceLocator &, const SourceLocator &) = default;
};

/**
 * @brief Persisted source descriptor. `nullopt` on `LogConfiguration::source`
 * means "no source bound". On load the app may re-open this; rebind
 * failure is non-fatal (columns and filters still apply). Legacy JSON
 * using the pre-widening `"locator"` field loses its source binding but
 * keeps the rest, courtesy of `error_on_unknown_keys=false` (see
 * `log_configuration_glaze_opts.hpp`).
 *
 * Locators are stored as `SourceLocator` values so display path and
 * dedup key cannot drift apart. JSON still uses the parallel
 * `locators` / `locatorDedupKeys` arrays; glaze maps them through
 * `ReplaceDisplayPaths` / `ReplaceDedupKeys`.
 */
struct Source
{
    enum class Kind
    {
        File,
        NetworkStream,
        /**
         * @brief Standard-input pipe (`StructuredLogViewer -` / `--stdin`).
         * One-shot per session; not persisted for auto-reopen
         * (see `MainWindow::ShouldAutoSaveSession`). Locator is
         * the synthetic display name `<stdin>`.
         */
        Stdin
    };

    /**
     * @brief Parser the source was opened with. Persisted because
     * network streams have nothing to sniff at restore time and
     * live-tail sessions commit to a parser before the first
     * byte arrives. Defaults to `Json` for a fresh `Source`.
     *
     * Serialised as stable strings (`"json"`, `"logfmt"`, `"csv"`,
     * ...) by `log_configuration_glaze_meta.hpp`. Append at the
     * end, never reorder existing values.
     */
    enum class Format
    {
        Json,
        Logfmt,
        Csv,
        /**
         * @brief PCRE2 regex parser. The pattern is carried in
         * `regexPattern` below; the parser refuses to run
         * without one when `format == Regex`.
         */
        Regex
    };

    Kind kind = Kind::File;
    Format format = Format::Json;
    std::vector<SourceLocator> locators;
    /**
     * @brief PCRE2 pattern with `(?<Name>...)` named capture groups.
     * Only meaningful when `format == Format::Regex`; empty
     * otherwise. Persisted so reopened sessions / network
     * streams keep parsing under the same template. The
     * matching template name (if any) is resolved at display
     * time via `loglib::FindTemplateByPattern`.
     */
    std::string regexPattern;

    /**
     * @brief Whether future file additions should include rotation siblings.
     * Restores use the persisted `locators` without rescanning.
     * Missing values default to `true` for older configurations.
     */
    bool followRotationSiblings = true;

    /**
     * @brief Staging for `locatorDedupKeys` that arrive before `locators`
     * during JSON load. Not serialized. `ReplaceDisplayPaths` consumes
     * and clears it; keys without a display-path array never become
     * locators.
     */
    std::vector<std::string> pendingDedupKeys;

    /**
     * @brief Replaces display paths, keeping overlapping dedup keys.
     *
     * Display-path count is canonical. Used by JSON load; either
     * field may arrive first. Staged `pendingDedupKeys` are applied
     * by index and then discarded.
     */
    void ReplaceDisplayPaths(std::vector<std::string> paths);

    /**
     * @brief Applies dedup keys by index.
     *
     * If locators are still empty, keys are staged on
     * `pendingDedupKeys` until `ReplaceDisplayPaths` sets the count.
     * Extra keys are ignored once display paths exist. Keys without a
     * following display-path array never become locators.
     */
    void ReplaceDedupKeys(std::vector<std::string> keys);

    /** @brief Display paths in locator order, for JSON write. */
    [[nodiscard]] std::vector<std::string> DisplayPaths() const;

    /** @brief Dedup keys in locator order, for JSON write. */
    [[nodiscard]] std::vector<std::string> DedupKeys() const;
};

/**
 * @brief "Source is actionable" predicate. Centralises the
 * `has_value() && !locators.empty()` gate so the half-checked form
 * can't sneak through one call site at a time.
 */
[[nodiscard]] inline bool HasLocators(const std::optional<Source> &source) noexcept
{
    return source.has_value() && !source->locators.empty();
}

/**
 * @brief Append a locator as a single `SourceLocator`. Call sites that
 * add a source path go through this helper (or `ClearLocators`).
 * @p dedupKey is taken pre-computed because canonicalisation lives in
 * the application layer (the library has no Qt dependency).
 */
inline void AppendLocator(Source &target, std::string displayPath, std::string dedupKey)
{
    target.locators.emplace_back(std::move(displayPath), std::move(dedupKey));
}

/** @brief Drop every locator. */
inline void ClearLocators(Source &target)
{
    target.locators.clear();
    target.pendingDedupKeys.clear();
}

/** @brief True when @p key matches any locator's dedup key. */
[[nodiscard]] inline bool ContainsDedupKey(const Source &source, std::string_view key) noexcept
{
    return std::ranges::any_of(
        source.locators, [key](const SourceLocator &locator) { return locator.dedupKey == key; }
    );
}

} // namespace loglib
