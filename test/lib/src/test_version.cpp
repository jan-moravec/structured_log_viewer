#include <loglib/core/version.hpp>

#include <catch2/catch_all.hpp>

#include <string_view>

#ifndef LOGLIB_TEST_PROJECT_VERSION
#error "LOGLIB_TEST_PROJECT_VERSION is set by test/lib/CMakeLists.txt"
#endif

TEST_CASE("Version matches the project version CMake sets", "[version]")
{
    const std::string_view version = loglib::Version();
    CHECK_FALSE(version.empty());
    CHECK(version == std::string_view(LOGLIB_TEST_PROJECT_VERSION));
}
