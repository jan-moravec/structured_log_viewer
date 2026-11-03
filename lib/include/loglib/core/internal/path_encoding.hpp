/**
 * @brief Converts between file system paths and UTF-8 text.
 *
 * Paths are stored as UTF-8 in session files and error messages. These helpers convert both ways and open files with
 * wide-character calls on Windows, so non-ASCII names work on every platform.
 *
 * This is part of the core module.
 */
#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace loglib::internal
{

/** @brief Returns the path as UTF-8 text. Unlike path::string it keeps non-ASCII characters in Windows paths. */
[[nodiscard]] inline std::string PathToUtf8(const std::filesystem::path &path)
{
    const auto u8 = path.u8string();
    return {reinterpret_cast<const char *>(u8.data()), u8.size()};
}

/** @brief Turns UTF-8 text into a path. */
[[nodiscard]] inline std::filesystem::path Utf8ToPath(std::string_view utf8)
{
    return std::filesystem::path{std::u8string{reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()}};
}

/**
 * @brief Opens a file for binary reading, with the wide path on Windows. Returns null and sets errno on failure. The
 * caller closes the file.
 */
[[nodiscard]] inline std::FILE *OpenFileForBinaryRead(const std::filesystem::path &path) noexcept
{
#ifdef _WIN32
    std::FILE *fp = nullptr;
    if (_wfopen_s(&fp, path.native().c_str(), L"rb") != 0)
    {
        return nullptr;
    }
    return fp;
#else
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) -- caller closes the FILE*.
    return std::fopen(path.c_str(), "rb");
#endif
}

/**
 * @brief Opens a file for binary writing, with the wide path on Windows. Returns null on failure. The caller closes
 * the file.
 */
[[nodiscard]] inline std::FILE *OpenFileForBinaryWrite(const std::filesystem::path &path) noexcept
{
#ifdef _WIN32
    std::FILE *fp = nullptr;
    if (_wfopen_s(&fp, path.native().c_str(), L"wb") != 0)
    {
        return nullptr;
    }
    return fp;
#else
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory) -- caller closes the FILE*.
    return std::fopen(path.c_str(), "wb");
#endif
}

} // namespace loglib::internal
