#include <loglib/file_line_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_line.hpp>
#include <loglib/parsers/json_parser.hpp>

#include <test_common/json_log_line.hpp>

#include <glaze/glaze.hpp>

#include <memory>
#include <vector>

class TestJsonLogFile
{
public:
    using Line = test_common::JsonLogLine;

    TestJsonLogFile(std::string filePath = FILE_PATH);
    TestJsonLogFile(Line line, std::string filePath = FILE_PATH);
    TestJsonLogFile(std::vector<Line> lines, std::string filePath = FILE_PATH);
    ~TestJsonLogFile() noexcept;

    const std::string &GetFilePath() const;
    void WriteToFile(std::vector<Line> lines);
    const std::vector<Line> &Lines() const;
    const std::vector<std::string> &StringLines() const;
    const std::vector<glz::generic_sorted_u64> &JsonLines() const;

private:
    static constexpr char FILE_PATH[] = "test.json";
    std::string mFilePath;
    std::vector<Line> mLines;
    std::vector<std::string> mStringLines;
    std::vector<glz::generic_sorted_u64> mJsonLines;
};

class TestLogConfiguration
{
public:
    TestLogConfiguration(std::string filePath = FILE_PATH);
    ~TestLogConfiguration() noexcept;

    const std::string &GetFilePath() const;
    void Write(const loglib::LogConfiguration &configuration) const;

private:
    static constexpr char FILE_PATH[] = "test_config.json";
    std::string mFilePath;
};

class TestLogFile
{
public:
    TestLogFile(std::string filePath = FILE_PATH);
    ~TestLogFile() noexcept;

    const std::string &GetFilePath() const;
    void Write(const std::string &content) const;
    std::unique_ptr<loglib::LogFile> CreateLogFile() const;

    /// Convenience helper that wraps `CreateLogFile()` in a
    /// `FileLineSource`. Used by tests that need to pass a `LineSource&`
    /// into a `LogLine` ctor.
    std::unique_ptr<loglib::FileLineSource> CreateFileLineSource() const;

private:
    static constexpr char FILE_PATH[] = "test_file.json";
    std::string mFilePath;
};

void InitializeTimezoneData();
