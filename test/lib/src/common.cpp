#include "common.hpp"

#include <loglib/log_file.hpp>
#include <loglib/log_processing.hpp>

#include <catch2/catch_all.hpp>
#include <date/tz.h>

#include <utility>

using namespace loglib;

TestJsonLogFile::TestJsonLogFile(std::string filePath)
    : mFilePath(std::move(filePath))
{
    const std::ofstream file(mFilePath);
    REQUIRE(file.is_open());
}

TestJsonLogFile::TestJsonLogFile(Line line, std::string filePath)
    : TestJsonLogFile(std::move(filePath))
{
    WriteToFile(std::vector<Line>{std::move(line)});
}

TestJsonLogFile::TestJsonLogFile(std::vector<Line> lines, std::string filePath)
    : TestJsonLogFile(std::move(filePath))
{
    WriteToFile(std::move(lines));
}

TestJsonLogFile::~TestJsonLogFile()
{
    std::filesystem::remove(GetFilePath());
}

const std::string &TestJsonLogFile::GetFilePath() const
{
    return mFilePath;
}

void TestJsonLogFile::WriteToFile(std::vector<Line> lines)
{
    mLines.clear();
    mStringLines.clear();
    mJsonLines.clear();

    std::ofstream file(GetFilePath(), std::ios::app);
    if (file.is_open())
    {
        for (const auto &line : lines)
        {
            file << line.ToString() << '\n';
            line.Parse(mStringLines, mJsonLines);
        }
        mLines = std::move(lines);
    }
    else
    {
        throw std::runtime_error("Failed to open test JSON file: " + std::string(GetFilePath()));
    }
}

const std::vector<TestJsonLogFile::Line> &TestJsonLogFile::Lines() const
{
    return mLines;
}

const std::vector<std::string> &TestJsonLogFile::StringLines() const
{
    return mStringLines;
}

const std::vector<glz::generic_sorted_u64> &TestJsonLogFile::JsonLines() const
{
    return mJsonLines;
}

TestLogConfiguration::TestLogConfiguration(std::string filePath)
    : mFilePath(std::move(filePath))
{
    const std::ofstream file(GetFilePath());
    REQUIRE(file.is_open());
}

TestLogConfiguration::~TestLogConfiguration()
{
    std::filesystem::remove(GetFilePath());
}

const std::string &TestLogConfiguration::GetFilePath() const
{
    return mFilePath;
}

void TestLogConfiguration::Write(const LogConfiguration &configuration) const
{
    std::ofstream file(GetFilePath());
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
    : mFilePath(std::move(filePath))
{
    const std::ofstream file(GetFilePath(), std::ios::binary);
    REQUIRE(file.is_open());
}

void TestLogFile::Write(const std::string &content) const
{
    std::ofstream file(GetFilePath(), std::ios::binary);
    REQUIRE(file.is_open());
    file << content;
}

TestLogFile::~TestLogFile()
{
    std::filesystem::remove(GetFilePath());
}

std::unique_ptr<loglib::LogFile> TestLogFile::CreateLogFile() const
{
    // Binary stream so streampos matches LogFile's byte offsets on every
    // platform (no CRLF translation).
    std::ifstream file(GetFilePath(), std::ios::binary);
    auto logFile = std::make_unique<LogFile>(GetFilePath());

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
