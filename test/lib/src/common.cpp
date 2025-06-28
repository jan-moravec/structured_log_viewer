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

const std::vector<glz::json_t> &TestJsonLogFile::JsonLines() const
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
    std::ofstream file(GetFilePath());
    REQUIRE(file.is_open());
}

void TestLogFile::Write(const std::string &content) const
{
    std::ofstream file(GetFilePath());
    REQUIRE(file.is_open());
    file << content;
}

TestLogFile::~TestLogFile()
{
    std::filesystem::remove(GetFilePath());
}

std::unique_ptr<loglib::LogFile> TestLogFile::CreateLogFile() const
{
    std::ifstream file(GetFilePath());
    std::string line;
    auto logFile = std::make_unique<LogFile>(GetFilePath());

    while (std::getline(file, line))
    {
        logFile->CreateReference(file.tellg());
    }

    return logFile;
}

void InitializeTimezoneData()
{
    static const auto TZ_DATA = std::filesystem::path("tzdata");
    std::filesystem::path path = std::filesystem::current_path();
    while (true)
    {
        const auto tzdataPath = path / TZ_DATA;
        if (std::filesystem::exists(tzdataPath))
        {
            loglib::Initialize(tzdataPath);
            break;
        }
        path = path.parent_path();
        REQUIRE(!path.empty());
    }
}

TestJsonLogFile::Line::Line(const char *line)
{
    glz::json_t json;
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

TestJsonLogFile::Line::Line(glz::json_t json) : data(std::move(json))
{
}

std::string TestJsonLogFile::Line::ToString() const
{
    return std::visit(
        [](const auto &data) -> std::string {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, glz::json_t>)
            {
                const auto result = glz::write_json(data);
                return *result;
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                return data;
            }
        },
        data
    );
}

void TestJsonLogFile::Line::Parse(std::vector<std::string> &strings, std::vector<glz::json_t> &jsons) const
{
    std::visit(
        [&](const auto &data) {
            using T = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<T, glz::json_t>)
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
