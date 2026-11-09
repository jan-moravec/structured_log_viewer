#include "common.hpp"

#include <loglib/schema/internal/log_configuration_glaze_meta.hpp>
#include <loglib/support/tzdata_search.hpp>
#include <loglib/time/time_zone_context.hpp>

#include <test_common/log_generator.hpp>
#include <test_common/temp_dir.hpp>

#include <catch2/catch_all.hpp>

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

using namespace loglib;

namespace
{

// Per-process scratch directory under `std::filesystem::temp_directory_path()`.
// Bare relative fixture names (e.g. `"test_file.json"`,
// `"regex_templates_<template>.log"`) resolve inside this directory so ad-hoc
// runs of a test binary from the workspace root can't leak stray files into
// the source tree. The random suffix keeps parallel `ctest -j` runs (and
// concurrent test binaries) from colliding on a fixed path. The directory is
// removed at process exit.
const std::filesystem::path &ScratchDir()
{
    static const test_common::TempDir SCRATCH_DIR("tests");
    return SCRATCH_DIR.Path();
}

// Resolve a caller-supplied path to a concrete on-disk location. Absolute
// paths pass through unchanged (a small number of tests deliberately point
// at fixed OS-temp locations); everything else lands inside `ScratchDir()`.
std::string ResolveScratchPath(std::string filePath)
{
    const std::filesystem::path fsPath(filePath);
    if (fsPath.is_absolute())
    {
        return filePath;
    }
    return (ScratchDir() / fsPath).string();
}

// Fixture helpers throw instead of using a Catch assertion so the benchmark executable, which has no Catch session,
// can use them too. A throw inside a test case still fails that case.
void ThrowIfNotOpen(bool isOpen)
{
    if (!isOpen)
    {
        throw std::runtime_error("could not open the fixture file for writing");
    }
}

// Open @p fsPath for binary writing (no '\n' translation, so offsets match
// `FileLineSource`), emit the format header if non-empty, and return the stream.
std::ofstream OpenStructuredFile(
    const std::filesystem::path &fsPath, const test_common::LogFormat &format, const test_common::RecordSchema &schema
)
{
    std::ofstream file(fsPath, std::ios::binary | std::ios::trunc);
    ThrowIfNotOpen(file.is_open());
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
    ThrowIfNotOpen(file.is_open());
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
    ThrowIfNotOpen(file.is_open());
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
    ThrowIfNotOpen(file.is_open());
}

void TestLogFile::Write(const std::string &content) const
{
    std::ofstream file(mFsPath, std::ios::binary);
    ThrowIfNotOpen(file.is_open());
    file << content;
}

TestLogFile::~TestLogFile() noexcept
{
    std::error_code ec;
    std::filesystem::remove(mFsPath, ec);
}

std::vector<std::uint64_t> LineEndOffsets(std::string_view text)
{
    // One offset per line feed; for an unterminated last line, size + 1 is the
    // virtual terminator (see FileLineSource::RawLine).
    std::vector<std::uint64_t> lineEnds;
    for (std::size_t pos = 0; pos < text.size(); ++pos)
    {
        if (text[pos] == '\n')
        {
            lineEnds.push_back(pos + 1);
        }
    }
    if (!text.empty() && text.back() != '\n')
    {
        lineEnds.push_back(text.size() + 1);
    }
    return lineEnds;
}

std::unique_ptr<loglib::FileLineSource> MemorySource(std::string text, std::string_view displayName)
{
    return loglib::FileLineSource::FromBytes(std::move(text), std::filesystem::path(displayName));
}

std::unique_ptr<loglib::FileLineSource> TestLogFile::CreateFileLineSource() const
{
    // Binary stream so the bytes match the mapping on every platform (no CRLF translation).
    std::ifstream file(mFsPath, std::ios::binary);
    const std::string content{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};

    auto source = std::make_unique<loglib::FileLineSource>(mFsPath);
    source->AppendLineOffsets(LineEndOffsets(content));
    return source;
}

std::unique_ptr<loglib::FileLineSource> MemoryLogFile::OpenSource() const
{
    return MemorySource(mText, mDisplayName);
}

std::unique_ptr<loglib::FileLineSource> MemoryLogFile::CreateFileLineSource() const
{
    auto source = MemorySource(mText, mDisplayName);
    source->AppendLineOffsets(LineEndOffsets(mText));
    return source;
}

std::unique_ptr<loglib::FileLineSource> MemoryStructuredLog::OpenSource() const
{
    return MemorySource(mText, mDisplayName);
}

MemoryStructuredLog::MemoryStructuredLog(
    std::vector<test_common::LogRecord> records,
    const test_common::LogFormat &format,
    const test_common::RecordSchema &schema,
    std::string displayName
)
    : mDisplayName(displayName.empty() ? "test" + std::string(format.suggestedExtension) : std::move(displayName)),
      mRecords(std::move(records))
{
    const std::string header = format.writeHeader(schema);
    if (!header.empty())
    {
        mText += header;
        mText += '\n';
    }
    for (const auto &record : mRecords)
    {
        const std::string line = format.writeLine(record);
        assert((line.empty() || line.back() != '\n') && "LogFormat::writeLine must not include trailing newline");
        mText += line;
        mText += '\n';
    }
}

std::filesystem::path FindTestTzdata()
{
    std::vector<std::filesystem::path> searched;
    const loglib::TzdataSearchRoots roots{.startDirectory = std::filesystem::current_path()};
    if (auto found = loglib::FindTzdataDirectory(roots, searched); !found.empty())
    {
        return found;
    }

    throw std::runtime_error(
        "FindTestTzdata(): no `tzdata` directory found along the "
        "ancestor chain of the current working directory (" +
        std::filesystem::current_path().string() +
        "). Run the binary via `ctest --preset local` or invoke it from a "
        "directory whose ancestor chain contains the staged `tzdata/` "
        "(typically `build/<preset>/bin/<config>/`)."
    );
    return {};
}

void InitializeTimezoneData()
{
    loglib::SetProcessDefaultTimeZone(loglib::TimeZoneContext::Load(FindTestTzdata()));
}
