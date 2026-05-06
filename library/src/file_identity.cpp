#include "loglib/internal/file_identity.hpp"

#include <cstdint>
#include <filesystem>

#ifdef _WIN32
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

#ifdef _WIN32
constexpr unsigned kFileIndexHighShift = 32U;

FileIdentity FromBhfi(const BY_HANDLE_FILE_INFORMATION &info) noexcept
{
    FileIdentity identity;
    identity.high = static_cast<uint64_t>(info.dwVolumeSerialNumber);
    identity.low = (static_cast<uint64_t>(info.nFileIndexHigh) << kFileIndexHighShift) |
                     static_cast<uint64_t>(info.nFileIndexLow);
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
#ifdef _WIN32
    // Full sharing so we don't disturb a concurrent producer.
    // FILE_FLAG_BACKUP_SEMANTICS lets the call also work on directories.
    HANDLE const handle = ::CreateFileW(
        path.c_str(),
        0, // metadata only
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
#ifdef _WIN32
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
