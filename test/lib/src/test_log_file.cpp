#include "common.hpp"

#include <loglib/log_file.hpp>

#include <catch2/catch_all.hpp>

using namespace loglib;

TEST_CASE("Successfully open a valid log file", "[LogFile]")
{
    // Create a temporary test file
    const TestLogFile testLogFile;
    testLogFile.Write("Line 1\nLine 2\nLine 3\n");

    // Verify successful file opening
    const std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
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
    const TestLogFile testLogFile;
    testLogFile.Write("Line 1\nLine 2");

    const std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();

    CHECK(logFile->GetLineCount() == 2);
    CHECK(logFile->GetLine(0) == "Line 1");
    CHECK(logFile->GetLine(1) == "Line 2");
}

TEST_CASE("Read lines from file with CRLF line endings", "[LogFile]")
{
    // Ensure byte offsets and CRLF stripping work regardless of the host's line-ending
    // conventions.
    const TestLogFile testLogFile;
    testLogFile.Write("Line 1\r\nLine 2\r\nLine 3\r\n");

    const std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();

    CHECK(logFile->GetLine(0) == "Line 1");
    CHECK(logFile->GetLine(1) == "Line 2");
    CHECK(logFile->GetLine(2) == "Line 3");
}

TEST_CASE("Throw runtime error when opening a non-existent file", "[LogFile]")
{
    CHECK_THROWS_AS(LogFile("non_existent_file.txt"), std::runtime_error);
}

// The mmap pointer must survive move so downstream `LogValue`s keep
// their `string_view`s into the content alive across ownership
// changes (e.g. `LogModel` taking ownership). Pins the contract in
// case a future `mio` upgrade drops it.
TEST_CASE("LogFile move preserves mmap pointer and content", "[LogFile][mmap-stability]")
{
    const TestLogFile testLogFile;
    testLogFile.Write("Line 1\nLine 2\nLine 3\n");

    auto original = testLogFile.CreateLogFile();
    REQUIRE(original != nullptr);

    const char *originalData = original->Data();
    const size_t originalSize = original->Size();
    REQUIRE(originalData != nullptr);
    REQUIRE(originalSize > 0);

    // Ground truth for post-move comparison.
    const std::string snapshot(originalData, originalSize);

    const LogFile moved = std::move(*original);

    // `mio::mmap_source` transfers its handle on move; guard against
    // a silent switch to a copying implementation.
    CHECK(moved.Data() == originalData);
    CHECK(moved.Size() == originalSize);

    // Reading through the moved-to instance confirms the mapping
    // survives the source's destructor below.
    const std::string movedSnapshot(moved.Data(), moved.Size());
    CHECK(movedSnapshot == snapshot);

    CHECK(moved.GetLine(0) == "Line 1");
    CHECK(moved.GetLine(1) == "Line 2");
    CHECK(moved.GetLine(2) == "Line 3");

    // Drop the original wrapper to exercise its (now-empty) dtor.
    original.reset();

    CHECK(moved.Data() == originalData);
    CHECK(moved.GetLine(0) == "Line 1");
}

// Regression: `AttachLifetimeAnchor` used to overwrite the previous
// anchor, dropping it before the mmap unmap (silent temp-file leak
// on Windows). Anchors are now composed and both survive to
// `~LogFile`. Sentinel dtors below pin the contract.
TEST_CASE("LogFile: AttachLifetimeAnchor composes multiple anchors", "[LogFile]")
{
    const TestLogFile testLogFile;
    testLogFile.Write("Line 1\nLine 2\n");

    struct Sentinel
    {
        int *counter;
        explicit Sentinel(int *c)
            : counter(c)
        {
        }
        ~Sentinel()
        {
            ++(*counter);
        }
        Sentinel(const Sentinel &) = delete;
        Sentinel &operator=(const Sentinel &) = delete;
        Sentinel(Sentinel &&) = delete;
        Sentinel &operator=(Sentinel &&) = delete;
    };

    int firstDestructed = 0;
    int secondDestructed = 0;
    {
        auto logFile = testLogFile.CreateLogFile();
        REQUIRE(logFile != nullptr);

        auto first = std::make_shared<Sentinel>(&firstDestructed);
        auto second = std::make_shared<Sentinel>(&secondDestructed);

        logFile->AttachLifetimeAnchor(first);
        logFile->AttachLifetimeAnchor(second);

        // Drop the local shared_ptrs; the LogFile's composed anchor
        // is now the sole owner of both sentinels.
        first.reset();
        second.reset();

        // Both must still be alive: LogFile hasn't been destroyed.
        CHECK(firstDestructed == 0);
        CHECK(secondDestructed == 0);
    }

    // LogFile dtor ran; both sentinels destroyed exactly once
    // (confirms the first anchor was not silently overwritten).
    CHECK(firstDestructed == 1);
    CHECK(secondDestructed == 1);
}
