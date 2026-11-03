/**
 * @brief Turns the paths a user selected into the list of files to open.
 *
 * ResolveSourcePlan expands each input into its rotation family when rotation history is on, lists every file once, and
 * returns the files in the order they should be loaded. With rotation history off, inputs come back in caller order,
 * duplicates included.
 *
 * This is part of the ingest module.
 */
#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace loglib
{

/**
 * @brief Controls how ResolveSourcePlan expands paths and which files it leaves out.
 *
 * Turn rotationHistory off to open exactly the paths given. Then the paths come back in the order given, including
 * duplicates, and none is marked as added automatically.
 *
 * suppressKeys lists files that must not be opened again. The keys are matched against CanonicalKeyForPath, and they
 * apply whether rotation history is on or not. When files are appended to what is already shown, pass the keys of the
 * files still visible. When the user replaces the content, or opens something that does not show earlier rows, pass an
 * empty span, so a file the user just asked for is not hidden. The span must stay valid during the call.
 */
struct SourcePlanOptions
{
    bool rotationHistory = true;
    std::span<const std::string> suppressKeys;
};

/**
 * @brief One file to open, in load order.
 *
 * autoAdded is true only for a numbered or dated companion that was found by searching and not named by the caller. A
 * primary, an unrelated file and a companion the caller named are never auto-added. That way a caller can count the
 * files that were found without counting the ones the user chose.
 */
struct ResolvedSource
{
    std::filesystem::path path;
    std::string canonicalKey;

    /**
     * @brief The primary file of the series this file belongs to. Empty when rotation history is off or the file
     * belongs to no series.
     */
    std::filesystem::path familyPrimary;

    bool autoAdded = false;
};

/**
 * @brief Works out which files to open, and in which order, for the paths a user selected.
 *
 * With rotation history on, paths are grouped with PartitionAsRotationSeries. Each series comes out oldest first with
 * its primary last. Series are ordered by their earliest input, and files that belong to no series follow in the order
 * given. The caller opens the list from front to back. Nothing is sorted by timestamp.
 *
 * Inputs that fall in the same series make one series, placed where the earliest input was. The search results and the
 * caller's own list are merged by canonical key, so a file named in both appears once. A listed file the search missed
 * is inserted before the primary. A path that does not exist is kept as a single unrelated entry, because this
 * function does not check that files can be read.
 *
 * @return The files in load order. Empty when paths is empty.
 */
[[nodiscard]] std::vector<ResolvedSource> ResolveSourcePlan(
    std::span<const std::filesystem::path> paths, const SourcePlanOptions &options
);

} // namespace loglib
