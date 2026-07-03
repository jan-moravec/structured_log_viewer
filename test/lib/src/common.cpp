#include "common.hpp"

#include <loglib/internal/log_configuration_glaze_meta.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_processing.hpp>

#include <test_common/log_generator.hpp>

#include <catch2/catch_all.hpp>
#include <date/tz.h>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

using namespace loglib;

namespace
{

// Per-process scratch directory under `std::filesystem::temp_directory_path()`.
// Bare relative fixture names (e.g. `"test_file.json"`,
// `"regex_templates_<template>.log"`) resolve inside this directory so ad-hoc
// runs of a test binary from the workspace root can't leak stray files into
// the source tree. The random suffix keeps parallel `ctest -j` runs (and
// concurrent test binaries) from colliding on a fixed path; the directory
// itself is not removed at process exit because individual fixtures already
// remove their own files in their dtors and the OS reclaims the empty dir
// on its usual temp-cleanup cadence.
const std::filesystem::path &ScratchDir()
{
    static const std::filesystem::path SCRATCH_DIR = [] {
        std::random_device rd;
        std::mt19937_64 gen(rd());
        const std::uint64_t suffix = gen();
        auto path = std::filesystem::temp_directory_path() / ("loglib_tests_" + std::to_string(suffix));
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        return path;
    }();
    return SCRATCH_DIR;
}

// Resolve a caller-supplied path to a concrete on-disk location. Absolute
// paths pass through unchanged (a small number of tests deliberately point
// at fixed OS-temp locations); everything else lands inside `ScratchDir()`.
std::string ResolveScratchPath(std::string filePath)
{
    std::filesystem::path fsPath(filePath);
    if (fsPath.is_absolute())
    {
        return filePath;
    }
    return (ScratchDir() / fsPath).string();
}

// Open @p fsPath for binary writing (no '\n' translation, so offsets match
// `LogFile`), emit the format header if non-empty, and return the stream.
std::ofstream OpenStructuredFile(
    const std::filesystem::path &fsPath, const test_common::LogFormat &format, const test_common::RecordSchema &schema
)
{
    std::ofstream file(fsPath, std::ios::binary | std::ios::trunc);
    REQUIRE(file.is_open());
    const std::string header = format.writeHeader(schema);
    if (!header.empty())
    {
        file << header << '\n';
    }
    return file;
}

// Resolve the on-disk fixture name: explicit `filePath` wins, otherwise
// `test<format-extension>` (so different-format fixtures don't collide).
// Both branches then land inside `ScratchDir()` via `ResolveScratchPath`.
std::string ResolveStructuredFilePath(std::string filePath, const test_common::LogFormat &format)
{
    if (!filePath.empty())
    {
        return ResolveScratchPath(std::move(filePath));
    }
    std::string derived = "test";
    derived.append(format.suggestedExtension);
    return ResolveScratchPath(std::move(derived));
}

} // namespace

TestStructuredLogFile::TestStructuredLogFile(
    std::vector<test_common::LogRecord> records,
    const test_common::LogFormat &format,
    const test_common::RecordSchema &schema,
    std::string filePath
)
    : mFilePath(ResolveStructuredFilePath(std::move(filePath), format)),
      mFsPath(mFilePath),
      mRecords(std::move(records)),
      mRecordCount(mRecords.size())
{
    std::ofstream file = OpenStructuredFile(mFsPath, format, schema);
    for (const auto &record : mRecords)
    {
        const std::string line = format.writeLine(record);
        assert((line.empty() || line.back() != '\n') && "LogFormat::writeLine must not include trailing newline");
        file << line << '\n';
    }
    if (!file.good())
    {
        // Remove the partial file before throwing — the dtor doesn't run
        // when the ctor exits via exception.
        std::error_code ec;
        std::filesystem::remove(mFsPath, ec);
        throw std::runtime_error("Failed to write structured log file: " + mFilePath);
    }
}

TestStructuredLogFile::TestStructuredLogFile(
    StreamedRecords streamed,
    const test_common::LogFormat &format,
    const test_common::RecordSchema &schema,
    std::string filePath
)
    : mFilePath(ResolveStructuredFilePath(std::move(filePath), format)),
      mFsPath(mFilePath),
      mRecordCount(streamed.count)
{
    std::ofstream file = OpenStructuredFile(mFsPath, format, schema);
    std::mt19937 rng(streamed.seed);
    for (std::size_t i = 0; i < streamed.count; ++i)
    {
        const test_common::LogRecord record = test_common::GenerateRandomLogRecord(rng, i, streamed.timestamps);
        const std::string line = format.writeLine(record);
        // `writeLine` must not include a trailing newline; otherwise we'd
        // emit blank-line records that every parser silently skips.
        assert((line.empty() || line.back() != '\n') && "LogFormat::writeLine must not include trailing newline");
        file << line << '\n';
    }
    if (!file.good())
    {
        std::error_code ec;
        std::filesystem::remove(mFsPath, ec);
        throw std::runtime_error("Failed to write structured log file: " + mFilePath);
    }
}

TestStructuredLogFile::~TestStructuredLogFile() noexcept
{
    std::error_code ec;
    std::filesystem::remove(mFsPath, ec);
}

const std::string &TestStructuredLogFile::GetFilePath() const
{
    return mFilePath;
}

std::size_t TestStructuredLogFile::RecordCount() const
{
    return mRecordCount;
}

const std::vector<test_common::LogRecord> &TestStructuredLogFile::Records() const
{
    return mRecords;
}

TestLogConfiguration::TestLogConfiguration(std::string filePath)
    : mFilePath(ResolveScratchPath(std::move(filePath))), mFsPath(mFilePath)
{
    const std::ofstream file(mFsPath);
    REQUIRE(file.is_open());
}

TestLogConfiguration::~TestLogConfiguration() noexcept
{
    std::error_code ec;
    std::filesystem::remove(mFsPath, ec);
}

const std::string &TestLogConfiguration::GetFilePath() const
{
    return mFilePath;
}

void TestLogConfiguration::Write(const LogConfiguration &configuration) const
{
    std::ofstream file(mFsPath);
    REQUIRE(file.is_open());
    std::string json;
    const auto error = glz::write_json(configuration, json);
    if (error)
    {
        throw std::runtime_error("Failed to serialize configuration: " + glz::format_error(error));
    }
    file << json;
}

const std::string &TestLogFile::GetFilePath() const
{
    return mFilePath;
}

TestLogFile::TestLogFile(std::string filePath)
    : mFilePath(ResolveScratchPath(std::move(filePath))), mFsPath(mFilePath)
{
    const std::ofstream file(mFsPath, std::ios::binary);
    REQUIRE(file.is_open());
}

void TestLogFile::Write(const std::string &content) const
{
    std::ofstream file(mFsPath, std::ios::binary);
    REQUIRE(file.is_open());
    file << content;
}

TestLogFile::~TestLogFile() noexcept
{
    std::error_code ec;
    std::filesystem::remove(mFsPath, ec);
}

std::unique_ptr<loglib::LogFile> TestLogFile::CreateLogFile() const
{
    // Binary stream so streampos matches LogFile's byte offsets on every
    // platform (no CRLF translation).
    std::ifstream file(mFsPath, std::ios::binary);
    auto logFile = std::make_unique<LogFile>(mFsPath);

    // Push one offset per '\n'; for an unterminated last line, push
    // `fileSize + 1` as the virtual terminator (see LogFile::GetLine).
    char ch = '\0';
    std::size_t pos = 0;
    while (file.get(ch))
    {
        ++pos;
        if (ch == '\n')
        {
            logFile->RegisterLineEnd(pos);
        }
    }
    if (pos > 0 && ch != '\n')
    {
        logFile->RegisterLineEnd(pos + 1);
    }

    return logFile;
}

std::unique_ptr<loglib::FileLineSource> TestLogFile::CreateFileLineSource() const
{
    return std::make_unique<loglib::FileLineSource>(CreateLogFile());
}

void InitializeTimezoneData()
{
    // Walk up the CWD ancestor chain for a sibling `tzdata/`. Both stop
    // conditions are required: `parent.empty()` for POSIX root, and
    // `parent == path` for Windows roots (`"C:\\"` is its own parent).
    static const auto TZ_DATA = std::filesystem::path("tzdata");
    std::filesystem::path path = std::filesystem::current_path();
    while (true)
    {
        const auto tzdataPath = path / TZ_DATA;
        std::error_code ec;
        if (std::filesystem::exists(tzdataPath, ec))
        {
            loglib::Initialize(tzdataPath);
            return;
        }
        const auto parent = path.parent_path();
        if (parent.empty() || parent == path)
        {
            break;
        }
        path = parent;
    }

    FAIL(
        "InitializeTimezoneData(): no `tzdata` directory found along the "
        "ancestor chain of the current working directory ("
        << std::filesystem::current_path().string()
        << "). Run the binary via `ctest --preset local` or invoke it from a "
           "directory whose ancestor chain contains the staged `tzdata/` "
           "(typically `build/<preset>/bin/<config>/`)."
    );
}
