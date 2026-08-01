#include "export_sink.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>

namespace slv::exports
{

namespace
{

std::string DescribeErrno(int err)
{
    // Portable, thread-safe errno formatting. `strerror` is deprecated
    // on MSVC (C4996) and not thread-safe on some libc's; the `_s` /
    // `_r` variants are the canonical replacements.
    constexpr size_t BUF_SIZE = 256;
    char buf[BUF_SIZE] = {0};
#ifdef _WIN32
    if (strerror_s(buf, BUF_SIZE, err) != 0)
    {
        return "unknown error";
    }
    return std::string(buf);
#else
    // Two `strerror_r` shapes exist and both compile under GCC / libc:
    //   * XSI (`_POSIX_C_SOURCE >= 200112L && !_GNU_SOURCE`) -- returns
    //     `int` (0 on success), writes into `buf`.
    //   * GNU (`_GNU_SOURCE`, the glibc default) -- returns `char *`,
    //     which may or may not point at `buf`; writing into `buf` is
    //     optional. Discarding the return value here would leave `buf`
    //     empty on glibc, giving a blank error message.
    //
    // Dispatch on the return type at compile time so both shapes work
    // regardless of which one the current build has enabled.
    auto result = strerror_r(err, buf, BUF_SIZE);
    if constexpr (std::is_same_v<decltype(result), char *>)
    {
        // GNU: pointer we must dereference. May be `buf` or a static
        // internal string.
        return std::string(result != nullptr ? result : "unknown error");
    }
    else
    {
        // XSI: int return; success writes into `buf`, non-zero means
        // the call itself failed (unknown errno, buffer too small).
        if (result != 0)
        {
            return "unknown error";
        }
        return std::string(buf);
    }
#endif
}

// Portable fopen wrapper: MSVC deprecates the plain `fopen` and
// prefers `_wfopen_s` for wide paths so this side of the sink
// keeps Unicode filenames intact on Windows.
std::FILE *OpenForWriteBinary(const std::filesystem::path &path)
{
#ifdef _WIN32
    std::FILE *file = nullptr;
    // `_wfopen_s` returns 0 on success; the errno on failure is
    // reported via the return value directly.
    const errno_t err = _wfopen_s(&file, path.wstring().c_str(), L"wb");
    if (err != 0 || file == nullptr)
    {
        errno = err;
        return nullptr;
    }
    return file;
#else
    return std::fopen(path.string().c_str(), "wb");
#endif
}

} // namespace

FileSink::FileSink(std::filesystem::path destination)
    : mDestination(std::move(destination))
{
    mTempPath = mDestination;
    mTempPath += ".tmp";
    mFile = OpenForWriteBinary(mTempPath);
    if (mFile == nullptr)
    {
        throw std::runtime_error(
            "Failed to open '" + mTempPath.string() + "' for writing: " + DescribeErrno(errno)
        );
    }
}

FileSink::~FileSink()
{
    if (mFinished)
    {
        return;
    }
    if (mFile != nullptr)
    {
        (void)std::fclose(mFile);
        mFile = nullptr;
    }
    // Best-effort cleanup: any error here means the temp file
    // outlives the process, which is a minor cosmetic issue; not
    // worth propagating from a destructor.
    std::error_code ec;
    std::filesystem::remove(mTempPath, ec);
}

void FileSink::Write(std::string_view bytes)
{
    if (mFile == nullptr)
    {
        throw std::runtime_error("FileSink::Write on a closed sink");
    }
    if (bytes.empty())
    {
        return;
    }
    const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), mFile);
    if (written != bytes.size())
    {
        const std::string reason = DescribeErrno(errno);
        throw std::runtime_error("Failed to write '" + mTempPath.string() + "': " + reason);
    }
}

void FileSink::Finish()
{
    if (mFinished)
    {
        return;
    }
    if (mFile == nullptr)
    {
        throw std::runtime_error("FileSink::Finish on a closed sink");
    }
    // Flush then close; both are checked because network shares
    // and deferred-write filesystems only surface I/O errors at
    // flush or close time.
    if (std::fflush(mFile) != 0)
    {
        const std::string reason = DescribeErrno(errno);
        // We're already throwing about the flush failure; the close
        // result is irrelevant beyond releasing the OS handle.
        (void)std::fclose(mFile);
        mFile = nullptr;
        throw std::runtime_error("Failed to flush '" + mTempPath.string() + "': " + reason);
    }
    if (std::fclose(mFile) != 0)
    {
        const std::string reason = DescribeErrno(errno);
        mFile = nullptr;
        throw std::runtime_error("Failed to close '" + mTempPath.string() + "': " + reason);
    }
    mFile = nullptr;

    std::error_code ec;
    std::filesystem::rename(mTempPath, mDestination, ec);
    if (ec)
    {
        // Rename can fail cross-device or if the destination is
        // held open by another process. Fall back to copy + remove
        // for the cross-device case; the held-open case still surfaces.
        if (ec == std::errc::cross_device_link)
        {
            std::error_code copyEc;
            std::filesystem::copy_file(
                mTempPath, mDestination, std::filesystem::copy_options::overwrite_existing, copyEc
            );
            if (copyEc)
            {
                std::error_code cleanupEc;
                std::filesystem::remove(mTempPath, cleanupEc);
                throw std::runtime_error(
                    "Failed to move '" + mTempPath.string() + "' to '" + mDestination.string() +
                    "': " + copyEc.message()
                );
            }
            std::error_code removeEc;
            std::filesystem::remove(mTempPath, removeEc);
        }
        else
        {
            std::error_code cleanupEc;
            std::filesystem::remove(mTempPath, cleanupEc);
            throw std::runtime_error(
                "Failed to rename '" + mTempPath.string() + "' to '" + mDestination.string() +
                "': " + ec.message()
            );
        }
    }
    mFinished = true;
}

} // namespace slv::exports
