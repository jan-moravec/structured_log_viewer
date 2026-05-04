#pragma once

#include <cstdint>
#include <filesystem>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace loglib::internal
{

/// Cross-platform file identity used by `TailingBytesProducer`
/// rotation detection. POSIX: `(st_dev, st_ino)`. Windows: volume
/// serial + `nFileIndexHigh:nFileIndexLow`.
///
/// On filesystems where the index isn't stable across rename
/// (FAT/exFAT, some SMB), identity comparison is unreliable. The
/// size-shrunk branch covers `copytruncate` there; rename-and-create
/// degrades to undetected — a documented limitation.
struct FileIdentity
{
    uint64_t high = 0; // POSIX: st_dev; Windows: volume serial
    uint64_t low = 0;  // POSIX: st_ino; Windows: file-index halves
    /// False values never compare equal — branch on `valid` first.
    bool valid = false;

    [[nodiscard]] friend bool operator==(const FileIdentity &lhs, const FileIdentity &rhs) noexcept
    {
        return lhs.valid && rhs.valid && lhs.high == rhs.high && lhs.low == rhs.low;
    }

    [[nodiscard]] friend bool operator!=(const FileIdentity &lhs, const FileIdentity &rhs) noexcept
    {
        return !(lhs == rhs);
    }
};

/// Sample identity of @p path without holding it open. POSIX: `stat`.
/// Windows: `OPEN_EXISTING` with full share flags, so a concurrent
/// writer is not disturbed. Returns `valid = false` on stat failure.
[[nodiscard]] FileIdentity FromPath(const std::filesystem::path &path) noexcept;

#if defined(_WIN32)
using NativeFileHandle = HANDLE;
#else
using NativeFileHandle = int; // POSIX fd
#endif

/// Sample identity of an open handle. POSIX: `fstat`. Windows:
/// `GetFileInformationByHandle`. Returns `valid = false` on failure.
[[nodiscard]] FileIdentity FromOpenHandle(NativeFileHandle handle) noexcept;

} // namespace loglib::internal
