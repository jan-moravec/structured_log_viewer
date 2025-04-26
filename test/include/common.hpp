#include <loglib/json_parser.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_line.hpp>

#include <nlohmann/json_fwd.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

class TestJsonLogFile
{
public:
    using Line = std::variant<std::string, nlohmann::json>;

    static const char *GetFilePath();

    TestJsonLogFile();

    explicit TestJsonLogFile(Line line);

    explicit TestJsonLogFile(std::vector<Line> lines);

    ~TestJsonLogFile();

    void WriteToFile(const std::vector<Line> &&lines);

    const std::vector<Line> &Lines() const;

    const std::vector<std::string> &StringLines() const;

    const std::vector<nlohmann::json> &JsonLines() const;

private:
    static constexpr char FILE_PATH[] = "test.json";
    std::vector<Line> mLines;
    std::vector<std::string> mStringLines;
    std::vector<nlohmann::json> mJsonLines;
};

class TestLogConfiguration
{
public:
    const std::string &GetFilePath() const;

    TestLogConfiguration(std::string filePath = FILE_PATH);

    ~TestLogConfiguration();

    void Write(const loglib::LogConfiguration &configuration);

private:
    static constexpr char FILE_PATH[] = "test_config.json";
    std::string mFilePath;
};

class TestLogFile
{
public:
    const std::string &GetFilePath() const;

    TestLogFile(std::string filePath = FILE_PATH);

    void Write(const std::string &content) const;

    ~TestLogFile();

    std::unique_ptr<loglib::LogFile> CreateLogFile() const;

private:
    static constexpr char FILE_PATH[] = "test_file.json";
    std::string mFilePath;
};
