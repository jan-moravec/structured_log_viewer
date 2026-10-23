#include "loglib/tzdata_search.hpp"

#include <cstdlib>
#include <system_error>
#include <utility>

namespace loglib
{

namespace
{

constexpr const char *TZDATA_DIRECTORY = "tzdata";

} // namespace

TzdataSearchRoots DefaultTzdataSearchRoots(const std::filesystem::path &executableDirectory)
{
    TzdataSearchRoots roots;
    roots.executableDirectory = executableDirectory;
    std::error_code error;
    roots.startDirectory = std::filesystem::current_path(error);
    if (error)
    {
        roots.startDirectory.clear();
    }
#if !defined(_WIN32) && !defined(__APPLE__)
    if (const char *appDir = std::getenv("APPDIR"); appDir != nullptr && appDir[0] != '\0')
    {
        roots.appImageDirectory = std::filesystem::path(appDir);
    }
#endif
    return roots;
}

std::filesystem::path FindTzdataDirectory(const TzdataSearchRoots &roots, std::vector<std::filesystem::path> &searched)
{
    auto consider = [&searched](std::filesystem::path candidate) {
        std::error_code error;
        const bool found = std::filesystem::is_directory(candidate, error) && !error;
        searched.push_back(std::move(candidate));
        return found;
    };

    if (!roots.executableDirectory.empty() && consider(roots.executableDirectory / TZDATA_DIRECTORY))
    {
        return searched.back();
    }

#ifdef __APPLE__
    if (!roots.executableDirectory.empty() &&
        consider(roots.executableDirectory.parent_path() / "Resources" / TZDATA_DIRECTORY))
    {
        return searched.back();
    }
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
    if (!roots.appImageDirectory.empty() && consider(roots.appImageDirectory / "usr" / "share" / TZDATA_DIRECTORY))
    {
        return searched.back();
    }
#endif

    std::filesystem::path walk = roots.startDirectory;
    while (!walk.empty())
    {
        if (consider(walk / TZDATA_DIRECTORY))
        {
            return searched.back();
        }
        const std::filesystem::path parent = walk.parent_path();
        if (parent.empty() || parent == walk)
        {
            break;
        }
        walk = parent;
    }
    return {};
}

} // namespace loglib
