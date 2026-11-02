#include "loglib/user_paths.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

#include <cstdlib>

namespace loglib
{
namespace
{

#ifndef _WIN32
[[nodiscard]] std::filesystem::path HomeDirectory()
{
    if (const std::optional<std::filesystem::path> home = EnvironmentPath("HOME"))
    {
        return *home;
    }
    // NOLINTNEXTLINE(concurrency-mt-unsafe): called once at startup, before any thread starts.
    const passwd *entry = getpwuid(geteuid());
    if (entry == nullptr || entry->pw_dir == nullptr || entry->pw_dir[0] == '\0')
    {
        return {};
    }
    return std::filesystem::path{entry->pw_dir};
}
#endif

void AppendApplicationIdentity(std::filesystem::path &path)
{
    if (!USER_DATA_ORGANIZATION_NAME.empty())
    {
        path /= std::string{USER_DATA_ORGANIZATION_NAME};
    }
    if (!USER_DATA_APPLICATION_NAME.empty())
    {
        path /= std::string{USER_DATA_APPLICATION_NAME};
    }
}

} // namespace

std::optional<std::filesystem::path> EnvironmentPath(std::string_view name)
{
#ifdef _WIN32
    const std::wstring wideName(name.begin(), name.end());
    const DWORD needed = GetEnvironmentVariableW(wideName.c_str(), nullptr, 0);
    if (needed == 0)
    {
        return std::nullopt;
    }
    std::wstring value(static_cast<size_t>(needed), L'\0');
    const DWORD written = GetEnvironmentVariableW(wideName.c_str(), value.data(), needed);
    if (written == 0)
    {
        return std::nullopt;
    }
    value.resize(written);
    return std::filesystem::path{std::move(value)};
#else
    const std::string nameText(name);
    // NOLINTNEXTLINE(concurrency-mt-unsafe): environment reads happen at startup, before any thread changes it.
    const char *value = std::getenv(nameText.c_str());
    if (value == nullptr || value[0] == '\0')
    {
        return std::nullopt;
    }
    return std::filesystem::path{value};
#endif
}

bool EnvironmentIsSet(std::string_view name)
{
#ifdef _WIN32
    const std::wstring wideName(name.begin(), name.end());
    return GetEnvironmentVariableW(wideName.c_str(), nullptr, 0) != 0 || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
#else
    const std::string nameText(name);
    // NOLINTNEXTLINE(concurrency-mt-unsafe): environment reads happen at startup, before any thread changes it.
    return std::getenv(nameText.c_str()) != nullptr;
#endif
}

std::filesystem::path UserDataDirectory()
{
#ifdef _WIN32
    std::optional<std::filesystem::path> base = EnvironmentPath("APPDATA");
    if (!base)
    {
        return {};
    }
    AppendApplicationIdentity(*base);
    return *base;
#elifdef __APPLE__
    std::filesystem::path home = HomeDirectory();
    if (home.empty())
    {
        return {};
    }
    std::filesystem::path base = home / "Library" / "Application Support";
    AppendApplicationIdentity(base);
    return base;
#else
    std::filesystem::path base;
    if (const std::optional<std::filesystem::path> xdg = EnvironmentPath("XDG_DATA_HOME"); xdg && xdg->is_absolute())
    {
        base = *xdg;
    }
    else
    {
        const std::filesystem::path home = HomeDirectory();
        if (home.empty())
        {
            return {};
        }
        base = home / ".local" / "share";
    }
    AppendApplicationIdentity(base);
    return base;
#endif
}

std::filesystem::path UserRegexTemplateDirectory()
{
    const std::filesystem::path base = UserDataDirectory();
    if (base.empty())
    {
        return {};
    }
    return base / "regex_templates";
}

} // namespace loglib
