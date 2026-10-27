#include <loglib/exports/export_sink.hpp>

#include <catch2/catch_all.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#else
#include <csignal>
#include <unistd.h>
#endif

namespace
{

class ScopedTempDir
{
public:
    ScopedTempDir()
    {
        std::error_code ec;
        mPath = std::filesystem::temp_directory_path(ec);
        mPath /= "slv_stream_sink_test";
        const auto nsecs = std::chrono::steady_clock::now().time_since_epoch().count();
        mPath += std::to_string(nsecs);
        std::filesystem::create_directories(mPath, ec);
    }

    ~ScopedTempDir() noexcept
    {
        try
        {
            std::error_code ec;
            std::filesystem::remove_all(mPath, ec);
        }
        catch (...) // NOLINT(bugprone-empty-catch)
        {
        }
    }

    ScopedTempDir(const ScopedTempDir &) = delete;
    ScopedTempDir &operator=(const ScopedTempDir &) = delete;
    ScopedTempDir(ScopedTempDir &&) = delete;
    ScopedTempDir &operator=(ScopedTempDir &&) = delete;

    [[nodiscard]] std::filesystem::path FilePath(std::string_view name) const
    {
        return mPath / std::filesystem::path(std::string(name));
    }

private:
    std::filesystem::path mPath;
};

std::filesystem::path TempSibling(const std::filesystem::path &path)
{
    auto temp = path;
    temp += ".tmp";
    return temp;
}

std::FILE *OpenBinaryWrite(const std::filesystem::path &path)
{
#ifdef _WIN32
    std::FILE *file = nullptr;
    const errno_t err = _wfopen_s(&file, path.wstring().c_str(), L"wb");
    if (err != 0 || file == nullptr)
    {
        throw std::runtime_error("failed to open temp file for writing");
    }
    return file;
#else
    const std::string native = path.string();
    std::FILE *file = std::fopen(native.c_str(), "wb");
    if (file == nullptr)
    {
        throw std::runtime_error("failed to open temp file for writing");
    }
    return file;
#endif
}

std::string ReadBinary(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

#ifndef _WIN32
class IgnoreSigpipe
{
public:
    IgnoreSigpipe()
    {
        struct sigaction action{};
        action.sa_handler = SIG_IGN;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGPIPE, &action, &mPrevious) != 0)
        {
            throw std::runtime_error("sigaction(SIGPIPE) failed");
        }
    }

    ~IgnoreSigpipe()
    {
        // NOLINTNEXTLINE(cert-err33-c)
        (void)sigaction(SIGPIPE, &mPrevious, nullptr);
    }

    IgnoreSigpipe(const IgnoreSigpipe &) = delete;
    IgnoreSigpipe &operator=(const IgnoreSigpipe &) = delete;

private:
    struct sigaction mPrevious{};
};

#endif

/// Write end of a pipe, as a `FILE *` the sink borrows.
class WritePipe
{
public:
    WritePipe()
    {
#ifdef _WIN32
        SECURITY_ATTRIBUTES security{};
        security.nLength = sizeof(security);
        security.bInheritHandle = FALSE;
        HANDLE readEnd = nullptr;
        HANDLE writeEnd = nullptr;
        if (::CreatePipe(&readEnd, &writeEnd, &security, 0) == 0)
        {
            throw std::runtime_error("CreatePipe failed");
        }
        mRead = readEnd;
        // NOLINTNEXTLINE(performance-no-int-to-ptr) -- CRT idiom; `_open_osfhandle` takes intptr_t by design.
        const int fd = ::_open_osfhandle(reinterpret_cast<intptr_t>(writeEnd), _O_BINARY);
        if (fd < 0)
        {
            ::CloseHandle(writeEnd);
            ::CloseHandle(readEnd);
            mRead = nullptr;
            throw std::runtime_error("_open_osfhandle failed");
        }
        mFile = ::_fdopen(fd, "wb");
        if (mFile == nullptr)
        {
            ::_close(fd);
            ::CloseHandle(readEnd);
            mRead = nullptr;
            throw std::runtime_error("_fdopen failed");
        }
#else
        int fds[2] = {-1, -1};
        if (::pipe(fds) != 0)
        {
            throw std::runtime_error("pipe failed");
        }
        mRead = fds[0];
        mFile = ::fdopen(fds[1], "wb");
        if (mFile == nullptr)
        {
            ::close(fds[0]);
            ::close(fds[1]);
            mRead = -1;
            throw std::runtime_error("fdopen failed");
        }
#endif
    }

    ~WritePipe() noexcept
    {
        CloseRead();
        if (mFile != nullptr)
        {
            // NOLINTNEXTLINE(cert-err33-c)
            (void)std::fclose(mFile);
            mFile = nullptr;
        }
    }

    WritePipe(const WritePipe &) = delete;
    WritePipe &operator=(const WritePipe &) = delete;

    void CloseRead() noexcept
    {
#ifdef _WIN32
        if (mRead != nullptr)
        {
            ::CloseHandle(mRead);
            mRead = nullptr;
        }
#else
        if (mRead >= 0)
        {
            ::close(mRead);
            mRead = -1;
        }
#endif
    }

    [[nodiscard]] std::FILE *File() const noexcept
    {
        return mFile;
    }

private:
#ifdef _WIN32
    HANDLE mRead = nullptr;
#else
    int mRead = -1;
#endif
    std::FILE *mFile = nullptr;
};

void RequireBrokenPipe(const auto &action)
{
    try
    {
        action();
    }
    catch (const loglib::exports::BrokenPipe &)
    {
        return;
    }
    catch (const std::runtime_error &ex)
    {
        FAIL(std::string("generic I/O error: ") + ex.what());
    }
    FAIL("closed read end did not throw");
}

} // namespace

TEST_CASE("StreamSink writes exact bytes and creates no temp file", "[exports][stream_sink]")
{
    const ScopedTempDir dir;
    const auto path = dir.FilePath("out.bin");
    const auto tempPath = TempSibling(path);
    const std::string_view payload("a\0b\n", 4);

    {
        std::FILE *file = OpenBinaryWrite(path);
        {
            loglib::exports::StreamSink sink(file);
            sink.Write(payload);
            sink.Finish();
            REQUIRE(sink.Finished());
            sink.Finish();
            REQUIRE(sink.Finished());
        }
        REQUIRE(std::fputc('!', file) != EOF);
        REQUIRE(std::fclose(file) == 0);
    }

    REQUIRE(ReadBinary(path) == std::string(payload) + "!");
    REQUIRE_FALSE(std::filesystem::exists(tempPath));

    const auto abandoned = dir.FilePath("abandoned.bin");
    {
        std::FILE *file = OpenBinaryWrite(abandoned);
        {
            loglib::exports::StreamSink sink(file);
            sink.Write(payload);
        }
        REQUIRE(std::fclose(file) == 0);
    }
    REQUIRE(std::filesystem::exists(abandoned));
    REQUIRE_FALSE(std::filesystem::exists(TempSibling(abandoned)));
}

TEST_CASE("StreamSink closed read end throws BrokenPipe", "[exports][stream_sink]")
{
    STATIC_REQUIRE_FALSE(std::is_base_of_v<std::runtime_error, loglib::exports::BrokenPipe>);

#ifndef _WIN32
    [[maybe_unused]] const IgnoreSigpipe ignoreSigpipe;
#endif

    SECTION("buffered write fails at Finish")
    {
        WritePipe pipe;
        pipe.CloseRead();
        loglib::exports::StreamSink sink(pipe.File());
        sink.Write(std::string_view("hi"));
        REQUIRE_FALSE(sink.Finished());
        RequireBrokenPipe([&] { sink.Finish(); });
    }

    SECTION("unbuffered write fails in Write")
    {
        WritePipe pipe;
        REQUIRE(std::setvbuf(pipe.File(), nullptr, _IONBF, 0) == 0);
        pipe.CloseRead();
        loglib::exports::StreamSink sink(pipe.File());
        RequireBrokenPipe([&] { sink.Write(std::string_view("hi")); });
    }
}
