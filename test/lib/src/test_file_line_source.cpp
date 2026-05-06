#include "common.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/log_file.hpp>

#include <catch2/catch_all.hpp>

#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

using loglib::FileLineSource;
using loglib::LogFile;

TEST_CASE("FileLineSource: forwards Path() to the wrapped LogFile", "[FileLineSource]")
{
    const TestLogFile testLogFile;
    testLogFile.Write("hello\n");
    auto logFile = testLogFile.CreateLogFile();

    FileLineSource source(std::move(logFile));
    CHECK(source.Path() == testLogFile.GetFilePath());
}

TEST_CASE("FileLineSource: rejects null files", "[FileLineSource]")
{
    CHECK_THROWS_AS(FileLineSource(std::unique_ptr<LogFile>{}), std::invalid_argument);
}

TEST_CASE("FileLineSource: RawLine returns CR-stripped per-line text", "[FileLineSource]")
{
    const TestLogFile testLogFile;
    testLogFile.Write("Line 1\nLine 2\nLine 3\n");
    auto logFile = testLogFile.CreateLogFile();

    FileLineSource source(std::move(logFile));

    CHECK(source.RawLine(0) == "Line 1");
    CHECK(source.RawLine(1) == "Line 2");
    CHECK(source.RawLine(2) == "Line 3");

    // Mirrors `LogFile::GetLine`'s out-of-range contract.
    CHECK_THROWS_AS(source.RawLine(3), std::out_of_range);
}

TEST_CASE("FileLineSource: ResolveMmapBytes indexes into the mmap data", "[FileLineSource]")
{
    const TestLogFile testLogFile;
    const std::string content = "abcdef\nghijkl\n";
    testLogFile.Write(content);
    auto logFile = testLogFile.CreateLogFile();

    const char *fileBegin = logFile->Data();
    REQUIRE(fileBegin != nullptr);
    REQUIRE(logFile->Size() == content.size());

    FileLineSource source(std::move(logFile));

    CHECK(source.BytesAreStable());
    CHECK_FALSE(source.SupportsEviction());

    CHECK(source.ResolveMmapBytes(0, 6, /*lineId=*/0) == "abcdef");
    CHECK(source.ResolveMmapBytes(7, 6, /*lineId=*/1) == "ghijkl");
    CHECK(source.ResolveMmapBytes(0, static_cast<uint32_t>(content.size()), /*lineId=*/0) == content);

    // Out-of-range queries return an empty view (not UB).
    CHECK(source.ResolveMmapBytes(0, 1024, /*lineId=*/0).empty());
    CHECK(source.ResolveMmapBytes(content.size() + 1, 1, /*lineId=*/0).empty());
}

TEST_CASE("FileLineSource: ResolveOwnedBytes indexes into the LogFile owned-string arena", "[FileLineSource]")
{
    const TestLogFile testLogFile;
    testLogFile.Write("x\n");
    auto logFile = testLogFile.CreateLogFile();

    const auto firstOffset = logFile->AppendOwnedStrings("first");
    const auto secondOffset = logFile->AppendOwnedStrings("second");
    REQUIRE(firstOffset == 0);
    REQUIRE(secondOffset == 5);

    FileLineSource source(std::move(logFile));

    CHECK(source.ResolveOwnedBytes(firstOffset, 5, /*lineId=*/0) == "first");
    CHECK(source.ResolveOwnedBytes(secondOffset, 6, /*lineId=*/0) == "second");

    // Out-of-range queries return an empty view.
    CHECK(source.ResolveOwnedBytes(0, 1024, /*lineId=*/0).empty());
    CHECK(source.ResolveOwnedBytes(1024, 1, /*lineId=*/0).empty());
}

TEST_CASE("FileLineSource: eviction is a no-op for finite mmap sources", "[FileLineSource]")
{
    const TestLogFile testLogFile;
    testLogFile.Write("a\nb\nc\n");
    auto logFile = testLogFile.CreateLogFile();

    FileLineSource source(std::move(logFile));

    CHECK(source.FirstAvailableLineId() == 0);
    source.EvictBefore(2);
    CHECK(source.FirstAvailableLineId() == 0);
    CHECK(source.RawLine(0) == "a");
    CHECK(source.RawLine(2) == "c");
}

TEST_CASE("FileLineSource: File() returns the underlying LogFile", "[FileLineSource]")
{
    const TestLogFile testLogFile;
    testLogFile.Write("hello\n");
    auto logFile = testLogFile.CreateLogFile();
    LogFile *raw = logFile.get();

    FileLineSource source(std::move(logFile));

    CHECK(&source.File() == raw);

    const auto &constSource = source;
    CHECK(&constSource.File() == raw);
}

TEST_CASE("FileLineSource: ReleaseFile transfers ownership but keeps the source resolvable", "[FileLineSource]")
{
    const TestLogFile testLogFile;
    testLogFile.Write("hello\n");
    auto logFile = testLogFile.CreateLogFile();
    LogFile *raw = logFile.get();

    FileLineSource source(std::move(logFile));

    auto released = source.ReleaseFile();
    REQUIRE(released != nullptr);
    CHECK(released.get() == raw);

    // The source still resolves against the released file (caller must
    // keep it alive, which we do via `released`).
    CHECK(&source.File() == raw);
    CHECK(source.RawLine(0) == "hello");

    // Releasing twice yields nullptr; the source still functions.
    auto secondRelease = source.ReleaseFile();
    CHECK(secondRelease == nullptr);
    CHECK(&source.File() == raw);
}
