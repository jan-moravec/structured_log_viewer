#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace loglib::internal
{

/// Return @p path as a UTF-8 encoded `std::string`.
///
/// Unlike `path::string()`, this preserves non-ASCII Windows paths.
[[nodiscard]] inline std::string PathToUtf8(const std::filesystem::path &path)
{
    const auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
}

/// Convert UTF-8 bytes to a portable filesystem path.
[[nodiscard]] inline std::filesystem::path Utf8ToPath(std::string_view utf8)
{
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size()));
}

/// Open a binary file for reading, using the wide path on Windows.
/// Returns `nullptr` and sets `errno` on failure.
[[nodiscard]] inline std::FILE *OpenFileForBinaryRead(const std::filesystem::path &path) noexcept
{
#if defined(_WIN32)
    std::FILE *fp = nullptr;
    if (_wfopen_s(&fp, path.native().c_str(), L"rb") != 0)
    {
        return nullptr;
    }
    return fp;
#else
    return std::fopen(path.c_str(), "rb");
#endif
}

/// Open a binary file for writing, using the wide path on Windows.
[[nodiscard]] inline std::FILE *OpenFileForBinaryWrite(const std::filesystem::path &path) noexcept
{
#if defined(_WIN32)
    std::FILE *fp = nullptr;
    if (_wfopen_s(&fp, path.native().c_str(), L"wb") != 0)
    {
        return nullptr;
    }
    return fp;
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

} // namespace loglib::internal
