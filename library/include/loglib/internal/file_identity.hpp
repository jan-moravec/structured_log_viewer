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

/// Cross-platform "is this the same file as last time?" identity used by
/// `TailingBytesProducer` rotation detection.
///
/// On POSIX the identity is `(st_dev, st_ino)`; on Windows it is the
/// volume serial number combined with `nFileIndexHigh:nFileIndexLow`
/// from `GetFileInformationByHandle`. We carry two 64-bit halves so a
/// future implementation that wants to keep the volume id separate
/// (e.g. for cross-volume rotation detection) does not require an ABI
/// change.
///
/// **FAT/exFAT and some SMB caveat.** On
/// filesystems where the file index is not stable across rename, the
/// identity comparison is unreliable. The size-shrunk branch (4.8.6.iii)
/// covers `copytruncate` on those filesystems; rename-and-create
/// degrades to undetected — a documented limitation.
struct FileIdentity
{
    /// Upper half of the identity. POSIX: `st_dev`; Windows: volume
    /// serial number.
    uint64_t high = 0;
    /// Lower half of the identity. POSIX: `st_ino`; Windows:
    /// `nFileIndexHigh:nFileIndexLow` combined.
    uint64_t low = 0;
    /// True iff the identity was successfully sampled. False values are
    /// not equal to any other value (incl. each other) — callers must
    /// branch on `valid` before comparing.
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

/// Sample the identity of @p path without holding any file open.
///
/// On POSIX uses `stat(2)`; on Windows opens with `FILE_SHARE_READ |
/// FILE_SHARE_WRITE | FILE_SHARE_DELETE` and `OPEN_EXISTING` so the call
/// does not interfere with a producer that holds the file open.
///
/// Returns a `FileIdentity` with `valid = false` when the path cannot
/// be stat'd (e.g. mid-rotation `ENOENT` / `ERROR_FILE_NOT_FOUND`).
[[nodiscard]] FileIdentity FromPath(const std::filesystem::path &path) noexcept;

#if defined(_WIN32)
using NativeFileHandle = HANDLE;
#else
using NativeFileHandle = int; // POSIX file descriptor
#endif

/// Sample the identity of an already-open native handle.
///
/// On POSIX takes a file descriptor and uses `fstat(2)`; on Windows
/// takes a `HANDLE` and uses `GetFileInformationByHandle`.
///
/// Returns a `FileIdentity` with `valid = false` when the handle is
/// invalid (`-1` / `INVALID_HANDLE_VALUE`) or the OS call fails.
[[nodiscard]] FileIdentity FromOpenHandle(NativeFileHandle handle) noexcept;

} // namespace loglib::internal
