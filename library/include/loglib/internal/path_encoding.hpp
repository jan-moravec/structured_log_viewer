#pragma once

#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>

namespace loglib::internal
{

/// Return @p path as a UTF-8 encoded `std::string`.
///
/// `std::filesystem::path::string()` returns the *native* encoding,
/// which on Windows is the Active Code Page (typically not UTF-8) and
/// silently mangles non-ASCII paths. `path::u8string()` returns
/// `std::u8string`, which we reinterpret as UTF-8 bytes.
///
/// Used for persisted locators and human-facing error strings that
/// mention paths, so both sides of a round-trip agree on the byte
/// layout.
[[nodiscard]] inline std::string PathToUtf8(const std::filesystem::path &path)
{
    const auto u8 = path.u8string();
    return std::string(reinterpret_cast<const char *>(u8.data()), u8.size());
}

/// Inverse of `PathToUtf8`. Accepts UTF-8 bytes (typically read out
/// of a JSON string) and returns a `path` that opens correctly on
/// both POSIX and Windows.
[[nodiscard]] inline std::filesystem::path Utf8ToPath(std::string_view utf8)
{
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t *>(utf8.data()), utf8.size())
    );
}

/// `fopen` that honours the wide-char path on Windows so non-ASCII
/// filenames open reliably. Returns `nullptr` on failure; the caller
/// must consult `errno` for a specific reason.
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

/// Companion to `OpenFileForBinaryRead` for write-binary opens.
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
