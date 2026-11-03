/**
 * @brief Finds the rotated files that belong to one log.
 *
 * Log rotation leaves series such as app.log, app.log.1 and app.log.2.gz. These helpers list the other files of a
 * series for a given primary file, and decide whether a set of selected paths forms one series. Each file gets a
 * normalized key so duplicates can be detected.
 *
 * This is part of the ingest module.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace loglib
{

/** @brief One file of a rotation series. */
struct RotatedFile
{
    /** @brief How the file was found. */
    enum class Origin
    {
        /** @brief The active log file. It is always the last file of a series. */
        Primary,
        /** @brief A file named like the primary with a number added, such as app.log.1, possibly compressed. */
        NumberedSuffix,
        /** @brief A file with a date added to the name, as a suffix or inside the name. */
        DatedSuffix,
        /** @brief A file the caller named that the search did not find. */
        CallerListed,
    };

    /** @brief The path as it should be opened. */
    std::filesystem::path path;

    /** @brief The key that identifies the file when removing duplicates. See CanonicalKeyForPath. */
    std::string canonicalKey;

    Origin origin = Origin::Primary;
};

/**
 * @brief A primary log file and its rotated companions, oldest first.
 *
 * Within each naming style the files are ordered from oldest to newest. Files the caller listed that have no rank come
 * before the primary, which is always last.
 */
struct RotationSeries
{
    std::filesystem::path primary;
    std::vector<RotatedFile> files;
};

/**
 * @brief Finds the numbered and dated companions of a log file in its folder.
 *
 * Numbered files are ordered by descending number and dated files by date. When both styles occur, a date is ranked by
 * its days since 1970 and a number N by 1,000,000,000 minus N. Files of equal rank may come in any order. The primary
 * is always in the result, last. Reading the folder is best effort: an error can give a partial result, and finding
 * nothing gives the primary alone.
 */
[[nodiscard]] RotationSeries EnumerateRotatedSiblings(const std::filesystem::path &primary);

/** @brief The result of splitting a list of paths into rotation series. */
struct PartitionedSelection
{
    /** @brief The series that were found, ordered by their earliest input path. */
    std::vector<RotationSeries> series;

    /** @brief The paths that belong to no series, in the order given and without duplicates. */
    std::vector<std::filesystem::path> residual;
};

/**
 * @brief Groups paths into rotation series.
 *
 * Each series is ordered like the result of EnumerateRotatedSiblings, and series are ordered by their earliest input
 * path. Looking at the file system is best effort.
 */
[[nodiscard]] PartitionedSelection PartitionAsRotationSeries(std::span<const std::filesystem::path> paths);

/**
 * @brief Builds a key that is the same for different spellings of one path.
 *
 * The key is an absolute path in UTF-8 with forward slashes, normalized without touching the file system. Letters are
 * lower-cased on Windows only. Failures are tolerated, so the key is best effort.
 */
[[nodiscard]] std::string CanonicalKeyForPath(const std::filesystem::path &path);

} // namespace loglib
