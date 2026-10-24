#include "platform.hpp"

#include <loglib/stop_token.hpp>

#include <catch2/catch_all.hpp>

#include <atomic>

TEST_CASE("the first cancel signal stops and the second terminates", "[cli][platform]")
{
    loglib::StopSource source;
    std::atomic<int> signals{0};
    CHECK(slp::AdvanceCancel(source, signals) == slp::CancelStep::Stop);
    CHECK(source.stop_requested());
    CHECK(slp::AdvanceCancel(source, signals) == slp::CancelStep::Terminate);
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
    CHECK(bad.error.contains("unavailable"));
}
