/**
 * @brief Identifies a file independently of its name.
 *
 * FileIdentity captures the operating system's identity of a file, so the tailing producer can tell whether the path
 * now points at the same file or at a new one after a rotation.
 *
 * This is part of the ingest module.
 */
#pragma once

#include <cstdint>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace loglib::internal
{

/**
 * @brief The operating system's identity of a file, used by TailingBytesProducer to detect rotation.
 *
 * On POSIX it is the device and the inode. On Windows it is the volume serial number and the file index.
 *
 * On file systems where the index changes when a file is renamed, such as FAT, exFAT and some network shares, the
 * comparison is unreliable. The size check still catches copy and truncate there, but a rename followed by a new file
 * is not detected.
 */
struct FileIdentity
{
    /** @brief The device on POSIX, the volume serial number on Windows. */
    uint64_t high = 0;

    /** @brief The inode on POSIX, the two halves of the file index on Windows. */
    uint64_t low = 0;

    /**
     * @brief False when the identity could not be read. Such an identity is never equal to another, so check this
     * first.
     */
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

/**
 * @brief Reads the identity of the file at a path without keeping it open.
 *
 * On Windows the file is opened with full sharing, so a writer is not disturbed. A failure gives an identity that is
 * not valid.
 */
[[nodiscard]] FileIdentity FromPath(const std::filesystem::path &path) noexcept;

#ifdef _WIN32
using NativeFileHandle = HANDLE;
#else
using NativeFileHandle = int;
#endif

/** @brief Reads the identity of an open file. A failure gives an identity that is not valid. */
[[nodiscard]] FileIdentity FromOpenHandle(NativeFileHandle handle) noexcept;

} // namespace loglib::internal
