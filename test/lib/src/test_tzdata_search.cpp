#include <loglib/tzdata_search.hpp>

#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>

#include <filesystem>
#include <fstream>
#include <system_error>
#include <vector>

TEST_CASE("FindTzdataDirectory prefers the executable directory", "[tzdata_search]")
{
    const test_common::TempDir dir("tzdata_search");
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
    const test_common::TempDir dir("tzdata_search");
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
    const test_common::TempDir dir("tzdata_search");
    const std::filesystem::path exeDir = dir.Path() / "bin";
    std::filesystem::create_directories(exeDir);
    {
        std::ofstream(exeDir / "tzdata") << "not a directory";
    }

    std::vector<std::filesystem::path> searched;
    const auto found = loglib::FindTzdataDirectory(loglib::TzdataSearchRoots{.executableDirectory = exeDir}, searched);
    CHECK(found.empty());
    // macOS also checks the bundle's Resources directory after this one.
    REQUIRE_FALSE(searched.empty());
    CHECK(searched.front() == exeDir / "tzdata");
}
