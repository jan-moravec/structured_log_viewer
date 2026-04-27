#include "common.hpp"

#include <loglib/log_processing.hpp>

#include <catch2/catch_all.hpp>
#include <date/tz.h>

#include <algorithm>
#include <random>
#include <utility>

using namespace loglib;

TestJsonLogFile::TestJsonLogFile(std::string filePath) : mFilePath(std::move(filePath))
{
    std::ofstream file(mFilePath);
    REQUIRE(file.is_open());
}

TestJsonLogFile::TestJsonLogFile(Line line, std::string filePath) : TestJsonLogFile(std::move(filePath))
{
    WriteToFile(std::vector<Line>{std::move(line)});
}

TestJsonLogFile::TestJsonLogFile(std::vector<Line> lines, std::string filePath) : TestJsonLogFile(std::move(filePath))
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

TestLogConfiguration::TestLogConfiguration(std::string filePath) : mFilePath(std::move(filePath))
{
    std::ofstream file(GetFilePath());
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

void TestLogConfiguration::Write(const LogConfiguration &configuration)
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

TestLogFile::TestLogFile(std::string filePath) : mFilePath(std::move(filePath))
{
    std::ofstream file(GetFilePath(), std::ios::binary);
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
    // Use a binary stream so std::streampos values match the byte offsets stored in
    // LogFile::mLineOffsets on every platform (no CRLF translation).
    std::ifstream file(GetFilePath(), std::ios::binary);
    auto logFile = std::make_unique<LogFile>(GetFilePath());

    // Scan for '\n' in the raw bytes and push one offset per
    // line. When the file does not end with a newline, push `fileSize + 1` as the virtual
    // terminator so GetLine's `stop - start - 1` size computation stays valid for the final
    // line.
    char ch = '\0';
    std::size_t pos = 0;
    while (file.get(ch))
    {
        ++pos;
        if (ch == '\n')
        {
            logFile->CreateReference(pos);
        }
    }
    if (pos > 0 && ch != '\n')
    {
        logFile->CreateReference(pos + 1);
    }

    return logFile;
}

void InitializeTimezoneData()
{
    // Walk up the ancestor chain of the current working directory looking
    // for a sibling `tzdata/` folder. The expected invocation patterns are
    // `ctest --preset local` (which runs each test from the build's working
    // directory) or running the binary directly from a directory whose
    // ancestor chain contains the staged `tzdata/` (`cmake/FetchDependencies
    // .cmake` puts one at `${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/tzdata`, i.e.
    // next to the .exe).
    //
    // Two stop conditions are required to keep the walk bounded:
    //   - `parent.empty()` covers POSIX where the parent of "/" is "".
    //   - `parent == path` covers Windows where
    //     `std::filesystem::path("C:\\").parent_path()` returns `C:\\`
    //     itself; the original loop relied on `REQUIRE(!path.empty())` and
    //     never terminated when no `tzdata` ancestor existed, manifesting
    //     as a hard hang of `tests.exe` whenever it was invoked from a CWD
    //     outside the build tree (e.g. `build/local/bin/Release/tests.exe`
    //     run from the repo root).
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

TestJsonLogFile::Line::Line(const char *line)
{
    glz::generic_sorted_u64 json;
    auto error = glz::read_json(json, line);
    if (!error)
    {
        data = std::move(json);
    }
    else
    {
        data = std::string(line);
    }
}

TestJsonLogFile::Line::Line(glz::generic_sorted_u64 json) : data(std::move(json))
{
}

std::string TestJsonLogFile::Line::ToString() const
{
    return std::visit(
        [](const auto &data) -> std::string {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, glz::generic_sorted_u64>)
            {
                return glz::write_json(data).value_or("");
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                return data;
            }
        },
        data
    );
}

void TestJsonLogFile::Line::Parse(std::vector<std::string> &strings, std::vector<glz::generic_sorted_u64> &jsons) const
{
    std::visit(
        [&](const auto &data) {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, glz::generic_sorted_u64>)
            {
                jsons.push_back(data);
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                strings.push_back(data);
            }
        },
        data
    );
}
