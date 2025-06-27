#include <loglib/json_parser.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_line.hpp>

#include <glaze/glaze.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

class TestJsonLogFile
{
public:
    struct Line
    {
        Line(const char *line);
        Line(glz::json_t json);

        using Type = std::variant<std::string, glz::json_t>;
        Type data;

        std::string ToString() const;
        void Parse(std::vector<std::string> &strings, std::vector<glz::json_t> &jsons) const;
    };

    TestJsonLogFile(std::string filePath = FILE_PATH);
    TestJsonLogFile(Line line, std::string filePath = FILE_PATH);
    TestJsonLogFile(std::vector<Line> lines, std::string filePath = FILE_PATH);
    ~TestJsonLogFile();

    const std::string &GetFilePath() const;
    void WriteToFile(std::vector<Line> lines);
    const std::vector<Line> &Lines() const;
    const std::vector<std::string> &StringLines() const;
    const std::vector<glz::json_t> &JsonLines() const;

private:
    static constexpr char FILE_PATH[] = "test.json";
    std::string mFilePath;
    std::vector<Line> mLines;
    std::vector<std::string> mStringLines;
    std::vector<glz::json_t> mJsonLines;
};

class TestLogConfiguration
{
public:
    TestLogConfiguration(std::string filePath = FILE_PATH);
    ~TestLogConfiguration();

    const std::string &GetFilePath() const;
    void Write(const loglib::LogConfiguration &configuration);

private:
    static constexpr char FILE_PATH[] = "test_config.json";
    std::string mFilePath;
};

class TestLogFile
{
public:
    TestLogFile(std::string filePath = FILE_PATH);
    ~TestLogFile();

    const std::string &GetFilePath() const;
    void Write(const std::string &content) const;
    std::unique_ptr<loglib::LogFile> CreateLogFile() const;

private:
    static constexpr char FILE_PATH[] = "test_file.json";
    std::string mFilePath;
};

void InitializeTimezoneData();
