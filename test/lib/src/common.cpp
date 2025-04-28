#include "common.hpp"

#include <loglib/log_processing.hpp>

#include <catch2/catch_all.hpp>
#include <date/tz.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <random>
#include <utility>

using namespace loglib;

const char *TestJsonLogFile::GetFilePath()
{
    return FILE_PATH;
}

TestJsonLogFile::TestJsonLogFile()
{
    std::ofstream file(GetFilePath());
    REQUIRE(file.is_open());
}

TestJsonLogFile::TestJsonLogFile(Line line) : TestJsonLogFile()
{
    WriteToFile({std::move(line)});
}

TestJsonLogFile::TestJsonLogFile(std::vector<Line> lines) : TestJsonLogFile()
{
    WriteToFile(std::move(lines));
}

TestJsonLogFile::~TestJsonLogFile()
{
    std::filesystem::remove(GetFilePath());
}

void TestJsonLogFile::WriteToFile(const std::vector<Line> &&lines)
{
    std::ofstream file(GetFilePath(), std::ios::app);
    if (file.is_open())
    {
        for (const auto &line : lines)
        {
            std::visit(
                [&file, this](const auto &line) {
                    using T = std::decay_t<decltype(line)>;
                    if constexpr (std::is_same_v<T, nlohmann::json>)
                    {
                        file << line.dump() << '\n';
                        mJsonLines.push_back(line);
                    }
                    else if constexpr (std::is_same_v<T, std::string>)
                    {
                        file << line << '\n';
                        mStringLines.push_back(line);
                    }
                },
                line
            );
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

const std::vector<nlohmann::json> &TestJsonLogFile::JsonLines() const
{
    return mJsonLines;
}

const std::string &TestLogConfiguration::GetFilePath() const
{
    return mFilePath;
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

void TestLogConfiguration::Write(const LogConfiguration &configuration)
{
    std::ofstream file(GetFilePath());
    REQUIRE(file.is_open());
    nlohmann::json json = configuration;
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
