#include "loglib/user_paths.hpp"

#include "loglib/ascii_case.hpp"
#include "loglib/internal/path_encoding.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
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

#include <cstdlib>
#endif

namespace loglib
{
namespace
{

#ifdef _WIN32
[[nodiscard]] std::optional<std::filesystem::path> EnvironmentPath(const wchar_t *name)
{
    const DWORD needed = GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0)
    {
        return std::nullopt;
    }
    std::wstring value(static_cast<size_t>(needed), L'\0');
    const DWORD written = GetEnvironmentVariableW(name, value.data(), needed);
    if (written == 0)
    {
        return std::nullopt;
    }
    value.resize(written);
    if (value.empty())
    {
        return std::nullopt;
    }
    return std::filesystem::path{std::move(value)};
}
#else
[[nodiscard]] std::optional<std::filesystem::path> EnvironmentPath(const char *name)
{
    const char *value = std::getenv(name);
    if (value == nullptr || value[0] == '\0')
    {
        return std::nullopt;
    }
    return std::filesystem::path{value};
}

[[nodiscard]] std::filesystem::path HomeDirectory()
{
    if (const std::optional<std::filesystem::path> home = EnvironmentPath("HOME"))
    {
        return *home;
    }
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

[[nodiscard]] bool IsJsonFile(const std::filesystem::path &path)
{
    return EqualsIgnoreCaseAscii(path.extension().string(), ".json");
}

constexpr size_t FILE_READ_CHUNK_BYTES = 4096;

[[nodiscard]] std::string ReadFile(std::FILE *file, bool &failed)
{
    std::string data;
    char buffer[FILE_READ_CHUNK_BYTES];
    while (true)
    {
        const size_t read = std::fread(buffer, 1, sizeof(buffer), file);
        if (read > 0)
        {
            data.append(buffer, read);
        }
        if (read < sizeof(buffer))
        {
            break;
        }
    }
    failed = std::ferror(file) != 0;
    return data;
}

} // namespace

std::filesystem::path UserDataDirectory()
{
#ifdef _WIN32
    std::optional<std::filesystem::path> base = EnvironmentPath(L"APPDATA");
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

RegexTemplateLoadResult LoadRegexTemplatesFromDirectory(const std::filesystem::path &directory)
{
    RegexTemplateLoadResult result;
    std::error_code existsError;
    const bool exists = std::filesystem::exists(directory, existsError);
    if (existsError || !exists)
    {
        if (existsError)
        {
            result.errors.push_back(
                "Cannot inspect regex template directory '" + internal::PathToUtf8(directory) +
                "': " + existsError.message()
            );
        }
        return result;
    }
    if (!std::filesystem::is_directory(directory, existsError) || existsError)
    {
        result.errors.push_back("Regex template path is not a directory: '" + internal::PathToUtf8(directory) + "'");
        return result;
    }

    std::vector<std::filesystem::path> files;
    try
    {
        for (const std::filesystem::directory_entry &entry : std::filesystem::directory_iterator{directory})
        {
            if (!entry.is_regular_file() || !IsJsonFile(entry.path()))
            {
                continue;
            }
            files.push_back(entry.path());
        }
    }
    catch (const std::filesystem::filesystem_error &error)
    {
        result.errors.push_back(
            "Cannot list regex template directory '" + internal::PathToUtf8(directory) + "': " + error.what()
        );
        return result;
    }

    std::ranges::sort(files, [](const std::filesystem::path &left, const std::filesystem::path &right) {
        return left.filename() < right.filename();
    });

    result.templates.reserve(files.size());
    for (const std::filesystem::path &path : files)
    {
        const std::string pathText = internal::PathToUtf8(path);
        std::FILE *file = internal::OpenFileForBinaryRead(path);
        if (file == nullptr)
        {
            const int error = errno;
            result.errors.push_back(
                "Cannot read regex template '" + pathText +
                "': " + std::error_code{error, std::generic_category()}.message()
            );
            continue;
        }
        bool failed = false;
        const std::string bytes = ReadFile(file, failed);
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,cert-err33-c)
        (void)std::fclose(file);
        if (failed)
        {
            result.errors.push_back("Cannot read regex template '" + pathText + "'");
            continue;
        }
        try
        {
            result.templates.push_back(ParseRegexTemplate(bytes));
        }
        catch (const std::exception &error)
        {
            result.errors.push_back("Cannot parse regex template '" + pathText + "': " + error.what());
        }
    }
    return result;
}

} // namespace loglib
