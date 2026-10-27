#pragma once

#include <filesystem>
#include <vector>

namespace loglib
{

/**
 * @brief Places `FindTzdataDirectory` looks for a staged `tzdata/` directory.
 *
 * Empty members are skipped.
 */
struct TzdataSearchRoots
{
    /** @brief Directory of the running executable. */
    std::filesystem::path executableDirectory;
    /** @brief First directory of the ancestor walk, normally the working directory. */
    std::filesystem::path startDirectory;
    /** @brief AppImage mount point (`$APPDIR`). Used on Linux only. */
    std::filesystem::path appImageDirectory;
};

/**
 * @brief Roots for a process started from @p executableDirectory.
 *
 * Adds the working directory as the walk start and, on Linux, `$APPDIR`
 * when it is set and non-empty.
 *
 * @param executableDirectory Directory of the running executable.
 */
[[nodiscard]] TzdataSearchRoots DefaultTzdataSearchRoots(const std::filesystem::path &executableDirectory);

/**
 * @brief Locates the staged `tzdata/` directory.
 *
 * Checks `executableDirectory/tzdata`, then on macOS
 * `executableDirectory/../Resources/tzdata`, then on Linux
 * `appImageDirectory/usr/share/tzdata`, then `tzdata` in
 * `startDirectory` and each of its ancestors. Only directories match.
 *
 * @param roots Search roots.
 * @param searched Receives every candidate, including misses, for diagnostics.
 * @return The first match, or an empty path.
 */
[[nodiscard]] std::filesystem::path FindTzdataDirectory(
    const TzdataSearchRoots &roots, std::vector<std::filesystem::path> &searched
);

} // namespace loglib
