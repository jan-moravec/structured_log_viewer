#include "platform.hpp"

#include <loglib/stop_token.hpp>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <filesystem>
#include <fstream>

namespace
{

class TempDir
{
public:
    TempDir()
        : mPath(std::filesystem::temp_directory_path() / "slp-platform-test")
    {
        std::filesystem::remove_all(mPath);
        std::filesystem::create_directories(mPath);
    }

    ~TempDir() noexcept
    {
        std::error_code error;
        std::filesystem::remove_all(mPath, error);
    }

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    [[nodiscard]] const std::filesystem::path &path() const
    {
        return mPath;
    }

private:
    std::filesystem::path mPath;
};

} // namespace

TEST_CASE("the first cancel signal stops and the second terminates", "[cli][platform]")
{
    loglib::StopSource source;
    std::atomic<int> signals{0};
    CHECK(slp::AdvanceCancel(source, signals) == slp::CancelStep::Stop);
    CHECK(source.stop_requested());
    CHECK(slp::AdvanceCancel(source, signals) == slp::CancelStep::Terminate);
}

TEST_CASE("tzdata search prefers the executable directory", "[cli][platform]")
{
    TempDir dir;
    const std::filesystem::path exeDir = dir.path() / "bin";
    const std::filesystem::path elsewhere = dir.path() / "walk";
    std::filesystem::create_directories(exeDir / "tzdata");
    std::filesystem::create_directories(elsewhere / "tzdata");
    std::ofstream(exeDir / "tzdata" / "zoneinfo") << "x";

    std::vector<std::filesystem::path> searched;
    const std::filesystem::path found = slp::FindTzdataDirectory(exeDir, elsewhere, {}, searched);
    CHECK(found == exeDir / "tzdata");
    REQUIRE_FALSE(searched.empty());
    CHECK(searched.front() == exeDir / "tzdata");
}

TEST_CASE("UTC suppresses the missing-tzdata warning", "[cli][platform]")
{
    const slp::TimeZoneSetup utc = slp::ResolveTimeZone("UTC", {});
    CHECK(utc.warning.empty());
    CHECK(utc.error.empty());
    CHECK(utc.context.IanaName() == "UTC");

    const slp::TimeZoneSetup local = slp::ResolveTimeZone("local", {});
    CHECK_FALSE(local.warning.empty());
    CHECK(local.error.empty());
    CHECK(local.context.IanaName() == "UTC");
}

TEST_CASE("an unknown IANA zone is a usage error when tzdata is present", "[cli][platform]")
{
    const std::filesystem::path tzdata = slp::CurrentExecutable().parent_path() / "tzdata";
    if (!std::filesystem::is_directory(tzdata))
    {
        SKIP("tzdata is not staged next to the test executable");
    }
    const slp::TimeZoneSetup bad = slp::ResolveTimeZone("Not/ARealZone", tzdata);
    CHECK(bad.error.find("unavailable") != std::string::npos);
}
