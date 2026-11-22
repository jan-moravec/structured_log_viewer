#include <loglib/config/source_descriptor.hpp>
#include <loglib/core/path_key.hpp>

#include <catch2/catch_all.hpp>

#include <filesystem>
#include <string>
#include <string_view>

using loglib::MakePathKey;

TEST_CASE("MakePathKey makes a relative path absolute", "[path_key]")
{
    const std::string key = MakePathKey("logs/app.log");
    CHECK(std::filesystem::path(key).is_absolute());
    CHECK(key.ends_with("logs/app.log"));
    CHECK(key == MakePathKey(std::filesystem::absolute("logs/app.log")));
}

TEST_CASE("MakePathKey resolves dots and doubled separators without touching the file system", "[path_key]")
{
    const auto root = std::filesystem::absolute("path_key_root");
    CHECK(MakePathKey(root / "a" / ".." / "b" / "." / "app.log") == MakePathKey(root / "b" / "app.log"));
    CHECK(MakePathKey(root / "does" / "not" / "exist.log").ends_with("does/not/exist.log"));
}

TEST_CASE("MakePathKey gives an empty key for an empty path", "[path_key]")
{
    CHECK(MakePathKey({}).empty());
}

TEST_CASE("MakePathKey produces UTF-8 bytes for non-ASCII paths", "[path_key]")
{
    const std::filesystem::path p(u8"/logs/\u65e5\u5fd7/app.log"); // /logs/日志/app.log
    std::string key;
    REQUIRE_NOTHROW(key = MakePathKey(p));
    // Match the six UTF-8 bytes for the two CJK characters.
    const std::string_view utf8Marker(reinterpret_cast<const char *>(u8"\u65e5\u5fd7"), 6);
    CHECK(key.contains(utf8Marker));
}

TEST_CASE("AppendLocator reads a non-ASCII path as UTF-8", "[path_key]")
{
    const std::filesystem::path p(u8"/logs/日志/app.log"); // /logs/日志/app.log
    const std::u8string utf8 = p.u8string();
    loglib::SourceDescriptor source;
    loglib::AppendLocator(source, std::string(utf8.begin(), utf8.end()));
    REQUIRE(source.locators.size() == 1);
    CHECK(source.locators.front().key == MakePathKey(p));
}

#ifdef _WIN32
TEST_CASE("MakePathKey uses forward slashes and folds case on Windows", "[path_key]")
{
    CHECK(MakePathKey("C:\\Logs\\App.LOG") == "c:/logs/app.log");
    CHECK(MakePathKey("C:/Logs/../Logs/App.log") == MakePathKey("c:\\logs\\app.log"));
}
#else
TEST_CASE("MakePathKey keeps the case on platforms with case-sensitive file systems", "[path_key]")
{
    CHECK(MakePathKey("/logs/App.log") != MakePathKey("/logs/app.log"));
    CHECK(MakePathKey("/logs/App.log") == "/logs/App.log");
}
#endif
