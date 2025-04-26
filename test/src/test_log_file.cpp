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

TEST_CASE("Throw runtime error when opening a non-existent file", "[LogFile]")
{
    CHECK_THROWS_AS(LogFile("non_existent_file.txt"), std::runtime_error);
}
