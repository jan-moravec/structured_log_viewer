#include "common.hpp"

#include <loglib/log_file.hpp>

#include <catch2/catch_all.hpp>
#include <fstream>

using namespace loglib;

TEST_CASE("Successfully open a valid log file", "[LogFile]")
{
    // Create a temporary test file
    TestLogFile testLogFile;
    testLogFile.Write("Line 1\nLine 2\nLine 3\n");

    // Verify successful file opening
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    CHECK(logFile->GetPath() == testLogFile.GetFilePath());

    // Verify we can read the lines
    CHECK(logFile->GetLine(0) == "Line 1");
    CHECK(logFile->GetLine(1) == "Line 2");
    CHECK(logFile->GetLine(2) == "Line 3");

    // Verify we can't read beyond the end of the file
    CHECK_THROWS_AS(logFile->GetLine(3), std::out_of_range);
}

TEST_CASE("Read last line without trailing newline", "[LogFile]")
{
    // The last line of a file may not end with '\n'; JsonParser handles that by pushing a
    // virtual terminator one byte past EOF. Verify GetLine still returns the full content.
    TestLogFile testLogFile;
    testLogFile.Write("Line 1\nLine 2");

    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();

    CHECK(logFile->GetLineCount() == 2);
    CHECK(logFile->GetLine(0) == "Line 1");
    CHECK(logFile->GetLine(1) == "Line 2");
}

TEST_CASE("Read lines from file with CRLF line endings", "[LogFile]")
{
    // Ensure byte offsets and CRLF stripping work regardless of the host's line-ending
    // conventions.
    TestLogFile testLogFile;
    testLogFile.Write("Line 1\r\nLine 2\r\nLine 3\r\n");

    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();

    CHECK(logFile->GetLine(0) == "Line 1");
    CHECK(logFile->GetLine(1) == "Line 2");
    CHECK(logFile->GetLine(2) == "Line 3");
}

TEST_CASE("Throw runtime error when opening a non-existent file", "[LogFile]")
{
    CHECK_THROWS_AS(LogFile("non_existent_file.txt"), std::runtime_error);
}

// PRD req. 4.1.6a — the mmap pointer returned by Data() must survive a move
// of the owning LogFile. This is what allows downstream LogValues to hold
// std::string_view`s into the file content for the entire LogFile lifetime,
// regardless of whether the LogFile is later moved out of the container that
// originally created it (e.g. LogModel taking ownership). If a future mio
// upgrade silently changes that contract, this test fails loudly.
TEST_CASE("LogFile move preserves mmap pointer and content", "[LogFile][mmap-stability]")
{
    TestLogFile testLogFile;
    testLogFile.Write("Line 1\nLine 2\nLine 3\n");

    auto original = testLogFile.CreateLogFile();
    REQUIRE(original != nullptr);

    const char *originalData = original->Data();
    const size_t originalSize = original->Size();
    REQUIRE(originalData != nullptr);
    REQUIRE(originalSize > 0);

    // Snapshot the bytes into a std::string while the original is still live —
    // the post-move comparison uses this as ground truth.
    const std::string snapshot(originalData, originalSize);

    LogFile moved = std::move(*original);

    // Pointer-stability under move. `mio::mmap_source` is documented to
    // transfer the underlying handle on move; we pin that fact here so an
    // accidental switch to a copying mmap implementation cannot ship.
    CHECK(moved.Data() == originalData);
    CHECK(moved.Size() == originalSize);

    // Read bytes through the moved-to instance to confirm the mapping is
    // still readable (i.e. the source instance's destructor — which runs
    // when `original` goes out of scope below — does not unmap the region).
    const std::string movedSnapshot(moved.Data(), moved.Size());
    CHECK(movedSnapshot == snapshot);

    // Per-line accessors must continue to work post-move.
    CHECK(moved.GetLine(0) == "Line 1");
    CHECK(moved.GetLine(1) == "Line 2");
    CHECK(moved.GetLine(2) == "Line 3");

    // Now drop the original wrapper so we exercise its (now-empty) destructor.
    original.reset();

    CHECK(moved.Data() == originalData);
    CHECK(moved.GetLine(0) == "Line 1");
}
