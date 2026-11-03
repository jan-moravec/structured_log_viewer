/**
 * @brief Finds the time zone database next to the program.
 *
 * The time zone database is staged as a folder beside the executable or installed in a system location. This header
 * lists the places to look and returns the first folder that holds the data.
 *
 * This is part of the support module.
 */
#pragma once

#include <filesystem>
#include <vector>

namespace loglib
{

/**
 * @brief The places FindTzdataDirectory searches for a folder of time zone data named tzdata. Empty members are
 * skipped.
 */
struct TzdataSearchRoots
{
    /** @brief The folder of the running program. */
    std::filesystem::path executableDirectory;
    /** @brief The folder where the walk up through parent folders starts, normally the working directory. */
    std::filesystem::path startDirectory;
    /** @brief The mount point of an AppImage, taken from the APPDIR variable. Used on Linux only. */
    std::filesystem::path appImageDirectory;
};

/**
 * @brief Builds the search places for a program started from the given folder. The working directory is where the walk
 * starts, and on Linux APPDIR is added when it is set and not empty.
 */
[[nodiscard]] TzdataSearchRoots DefaultTzdataSearchRoots(const std::filesystem::path &executableDirectory);

/**
 * @brief Finds the folder of time zone data that was installed with the program.
 *
 * It looks in this order: tzdata next to the program; on macOS, ../Resources/tzdata next to the program; on Linux,
 * usr/share/tzdata inside the AppImage; and then tzdata in the start folder and in each of its parent folders. Only
 * folders match. Every place that was tried, including those that did not match, is added to searched for diagnostics.
 * It returns the first match, or an empty path.
 */
[[nodiscard]] std::filesystem::path FindTzdataDirectory(
    const TzdataSearchRoots &roots, std::vector<std::filesystem::path> &searched
);

} // namespace loglib
