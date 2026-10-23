#include <loglib/tzdata_search.hpp>

#include <catch2/catch_all.hpp>

#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

namespace
{

class ScratchDirectory
{
public:
    ScratchDirectory()
        : mPath(std::filesystem::temp_directory_path() / "loglib-tzdata-search")
    {
        std::filesystem::remove_all(mPath);
        std::filesystem::create_directories(mPath);
    }

    ~ScratchDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(mPath, error);
    }

    ScratchDirectory(const ScratchDirectory &) = delete;
    ScratchDirectory &operator=(const ScratchDirectory &) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const noexcept
    {
        return mPath;
    }

private:
    std::filesystem::path mPath;
};

} // namespace

TEST_CASE("FindTzdataDirectory prefers the executable directory", "[tzdata_search]")
{
    const ScratchDirectory dir;
    const std::filesystem::path exeDir = dir.Path() / "bin";
    const std::filesystem::path walk = dir.Path() / "walk";
    std::filesystem::create_directories(exeDir / "tzdata");
    std::filesystem::create_directories(walk / "tzdata");

    std::vector<std::filesystem::path> searched;
    const auto found = loglib::FindTzdataDirectory(
        loglib::TzdataSearchRoots{.executableDirectory = exeDir, .startDirectory = walk}, searched
    );
    CHECK(found == exeDir / "tzdata");
    REQUIRE(searched.size() == 1);
    CHECK(searched.front() == exeDir / "tzdata");
}

TEST_CASE("FindTzdataDirectory walks up from the start directory", "[tzdata_search]")
{
    const ScratchDirectory dir;
    const std::filesystem::path nested = dir.Path() / "a" / "b";
    std::filesystem::create_directories(nested);
    std::filesystem::create_directories(dir.Path() / "tzdata");

    std::vector<std::filesystem::path> searched;
    const auto found = loglib::FindTzdataDirectory(loglib::TzdataSearchRoots{.startDirectory = nested}, searched);
    CHECK(found == dir.Path() / "tzdata");
    CHECK(searched.size() >= 3);
}

TEST_CASE("FindTzdataDirectory ignores a file named tzdata", "[tzdata_search]")
{
    const ScratchDirectory dir;
    const std::filesystem::path exeDir = dir.Path() / "bin";
    std::filesystem::create_directories(exeDir);
    {
        std::ofstream(exeDir / "tzdata") << "not a directory";
    }

    std::vector<std::filesystem::path> searched;
    const auto found = loglib::FindTzdataDirectory(loglib::TzdataSearchRoots{.executableDirectory = exeDir}, searched);
    CHECK(found.empty());
    CHECK(searched == std::vector<std::filesystem::path>{exeDir / "tzdata"});
}
