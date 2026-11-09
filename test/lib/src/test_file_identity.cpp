#include <loglib/ingest/internal/file_identity.hpp>

#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>

#include <filesystem>
#include <fstream>
#include <system_error>

using loglib::internal::FileIdentity;
using loglib::internal::FromPath;

namespace
{

void Touch(const std::filesystem::path &path)
{
    std::ofstream out(path, std::ios::binary);
    out << "x";
}

} // namespace

TEST_CASE("FromPath reads a valid identity of an existing file", "[file_identity]")
{
    const test_common::TempDir dir("identity");
    const auto path = dir.Path() / "a.log";
    Touch(path);

    const FileIdentity identity = FromPath(path);
    CHECK(identity.valid);
    CHECK(identity == FromPath(path));
}

TEST_CASE("FromPath gives the same identity through two paths", "[file_identity]")
{
    const test_common::TempDir dir("identity");
    std::filesystem::create_directories(dir.Path() / "sub");
    const auto path = dir.Path() / "a.log";
    Touch(path);

    const auto indirect = dir.Path() / "sub" / ".." / "a.log";
    CHECK(FromPath(path) == FromPath(indirect));

    std::error_code ec;
    const auto link = dir.Path() / "hard.log";
    std::filesystem::create_hard_link(path, link, ec);
    if (!ec)
    {
        CHECK(FromPath(path) == FromPath(link));
    }
}

TEST_CASE("FromPath tells two files apart", "[file_identity]")
{
    const test_common::TempDir dir("identity");
    const auto first = dir.Path() / "a.log";
    const auto second = dir.Path() / "b.log";
    Touch(first);
    Touch(second);

    CHECK(FromPath(first) != FromPath(second));
}

TEST_CASE("FromPath follows a file across a rename but not a replacement", "[file_identity]")
{
    const test_common::TempDir dir("identity");
    const auto path = dir.Path() / "a.log";
    const auto rotated = dir.Path() / "a.log.1";
    Touch(path);
    const FileIdentity before = FromPath(path);
    REQUIRE(before.valid);

    std::filesystem::rename(path, rotated);
    CHECK(FromPath(rotated) == before);
    CHECK_FALSE(FromPath(path).valid);

    Touch(path);
    CHECK(FromPath(path) != before);
}

TEST_CASE("FromPath of a missing file is not valid and equals nothing", "[file_identity]")
{
    const test_common::TempDir dir("identity");
    const FileIdentity missing = FromPath(dir.Path() / "missing.log");
    CHECK_FALSE(missing.valid);
    CHECK(missing != missing);
    CHECK(missing != FileIdentity{});

    const auto path = dir.Path() / "a.log";
    Touch(path);
    CHECK(missing != FromPath(path));
}
