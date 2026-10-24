#include "captured_console.hpp"
#include "console.hpp"
#include "options.hpp"

#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>

#include <cstdio>
#include <memory>
#include <string>

TEST_CASE("Console::Print reports a stream that cannot be written as an output failure", "[cli][console]")
{
    const test_common::TempDir dir("console");
    const std::string path = dir.Write("readonly.txt", "x").string();
    const auto closer = [](std::FILE *file) { (void)std::fclose(file); };
    const std::unique_ptr<std::FILE, decltype(closer)> readOnly(std::fopen(path.c_str(), "rb"), closer);
    REQUIRE(readOnly);
    const std::unique_ptr<std::FILE, decltype(closer)> err(slp::test::OpenTemporaryFile(), closer);
    REQUIRE(err);

    const slp::Console console(readOnly.get(), err.get(), "formats", slp::GlobalOptions{});
    CHECK(console.Print("payload\n") == slp::ExitCode::Output);
    CHECK(console.Print("") == slp::ExitCode::Success);

    std::rewind(err.get());
    std::string report(256, '\0');
    report.resize(std::fread(report.data(), 1, report.size(), err.get()));
    CHECK(report.contains("could not write to standard output"));
}
