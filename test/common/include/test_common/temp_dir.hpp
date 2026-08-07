#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>

namespace test_common
{

/// RAII scratch directory rooted under `std::filesystem::temp_directory_path()`.
/// Each instance owns a uniquely-suffixed subdirectory so parallel
/// `ctest -j` runs (or concurrent test binaries) don't collide.
/// Removes the directory (and everything in it) on destruction on
/// a best-effort basis; a failed cleanup only leaks a temp dir.
///
/// This is a header-only helper because the test targets that need
/// it are already header-heavy (Catch2, benchmarks); pulling it out
/// of two `test_*.cpp` files keeps the fixture behaviour identical.
class TempDir
{
public:
    explicit TempDir(std::string_view label = "tmp")
    {
        auto base = std::filesystem::temp_directory_path();
        std::random_device rd;
        std::mt19937_64 gen(rd());
        const std::uint64_t suffix = gen();
        mPath = base / (std::string("loglib_") + std::string(label) + "_test_" + std::to_string(suffix));
        std::error_code ec;
        std::filesystem::create_directories(mPath, ec);
        if (ec)
        {
            throw std::runtime_error("TempDir: failed to create scratch dir: " + mPath.string());
        }
    }

    // NOLINTNEXTLINE(bugprone-exception-escape)
    ~TempDir() noexcept
    {
        std::error_code ec;
        std::filesystem::remove_all(mPath, ec);
    }

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;
    TempDir(TempDir &&) = delete;
    TempDir &operator=(TempDir &&) = delete;

    /// Path to the scratch directory itself. Callers can compose
    /// their own file names with `Path() / "sub.log"` when they
    /// need finer control than `Write`.
    [[nodiscard]] const std::filesystem::path &Path() const noexcept
    {
        return mPath;
    }

    /// Write @p contents to @p name inside the scratch directory
    /// (binary mode; no line-ending translation). Returns the
    /// absolute path to the created file. Throws
    /// `std::runtime_error` on open / write failure so tests
    /// surface the reason instead of racing an empty file.
    [[nodiscard]] std::filesystem::path Write(const std::string &name, std::string_view contents) const
    {
        const auto path = mPath / name;
        std::ofstream out(path, std::ios::binary);
        if (!out.is_open())
        {
            throw std::runtime_error("TempDir::Write: could not open " + path.string());
        }
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!out.good())
        {
            throw std::runtime_error("TempDir::Write: write failed for " + path.string());
        }
        return path;
    }

private:
    std::filesystem::path mPath;
};

} // namespace test_common
