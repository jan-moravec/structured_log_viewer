#include "loglib/internal/file_identity.hpp"

#include <cstdint>
#include <filesystem>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace loglib::internal
{

namespace
{

#if defined(_WIN32)
FileIdentity FromBhfi(const BY_HANDLE_FILE_INFORMATION &info) noexcept
{
    FileIdentity identity;
    identity.high = static_cast<uint64_t>(info.dwVolumeSerialNumber);
    identity.low = (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | static_cast<uint64_t>(info.nFileIndexLow);
    identity.valid = true;
    return identity;
}
#else
FileIdentity FromStat(const struct stat &st) noexcept
{
    FileIdentity identity;
    identity.high = static_cast<uint64_t>(st.st_dev);
    identity.low = static_cast<uint64_t>(st.st_ino);
    identity.valid = true;
    return identity;
}
#endif

} // namespace

FileIdentity FromPath(const std::filesystem::path &path) noexcept
{
#if defined(_WIN32)
    // Open with full sharing so we do not interfere with a producer
    // holding the file open; FILE_FLAG_BACKUP_SEMANTICS so the call
    // works on directories too (cheap robustness — the caller passes
    // files, but a parent-dir poll could pass either).
    const HANDLE handle = ::CreateFileW(
        path.c_str(),
        0, // no read/write access needed; only metadata
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr
    );
    if (handle == INVALID_HANDLE_VALUE)
    {
        return FileIdentity{};
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(handle, &info))
    {
        ::CloseHandle(handle);
        return FileIdentity{};
    }
    ::CloseHandle(handle);
    return FromBhfi(info);
#else
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0)
    {
        return FileIdentity{};
    }
    return FromStat(st);
#endif
}

FileIdentity FromOpenHandle(NativeFileHandle handle) noexcept
{
#if defined(_WIN32)
    if (handle == INVALID_HANDLE_VALUE || handle == nullptr)
    {
        return FileIdentity{};
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!::GetFileInformationByHandle(handle, &info))
    {
        return FileIdentity{};
    }
    return FromBhfi(info);
#else
    if (handle < 0)
    {
        return FileIdentity{};
    }
    struct stat st{};
    if (::fstat(handle, &st) != 0)
    {
        return FileIdentity{};
    }
    return FromStat(st);
#endif
}

} // namespace loglib::internal
