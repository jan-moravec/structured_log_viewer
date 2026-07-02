#include <loglib/file_line_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_line.hpp>
#include <loglib/regex_templates.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>
#include <test_common/log_record.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

// Streaming-fixture spec: generate and write `count` random records on the
// fly, seeded for reproducibility. Used by the large benchmark so we don't
// materialize millions of `LogRecord`s in RAM. Pass a fixed `baseTime` via
// `bench::DeterministicBenchmarkTimestamps()` for byte-identical fixtures
// across runs and formats.
struct StreamedRecords
{
    std::size_t count = 0;
    std::uint32_t seed = test_common::MakeRandomSeed();
    test_common::TimestampPolicy timestamps{};
};

// RAII fixture that serializes `LogRecord`s through a `LogFormat` to disk
// and removes the file on destruction.
//   * materialized ctor: keeps the records so tests can assert on them;
//   * streaming ctor:    generates records on the fly, keeping only count.
//
// When `filePath` is empty, the on-disk name is `test<extension>` derived
// from the format, so simultaneous fixtures of different formats don't
// collide.
class TestStructuredLogFile
{
public:
    TestStructuredLogFile(
        std::vector<test_common::LogRecord> records,
        const test_common::LogFormat &format,
        const test_common::RecordSchema &schema = {},
        std::string filePath = {}
    );
    TestStructuredLogFile(
        StreamedRecords streamed,
        const test_common::LogFormat &format,
        const test_common::RecordSchema &schema = {},
        std::string filePath = {}
    );
    ~TestStructuredLogFile() noexcept;

    const std::string &GetFilePath() const;
    std::size_t RecordCount() const;
    // Populated only on the materialized path; empty for streaming fixtures.
    const std::vector<test_common::LogRecord> &Records() const;

private:
    std::string mFilePath;
    std::filesystem::path mFsPath;
    std::vector<test_common::LogRecord> mRecords;
    std::size_t mRecordCount = 0;
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
    std::filesystem::path mFsPath;
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
    std::filesystem::path mFsPath;
};

void InitializeTimezoneData();

/// Look up a shipped `loglib::RegexTemplate` by exact display name.
/// Small linear scan; the built-in registry is O(30) entries and this
/// only fires in test setup. Returns nullptr when @p name matches no
/// built-in, letting the caller REQUIRE non-null with a clear error
/// message. Shared between the round-trip synthesizer tests and the
/// per-template `[regex_parser][large]` benchmarks so both look up the
/// same source-of-truth pattern.
inline const loglib::RegexTemplate *FindTemplateByName(std::string_view name) noexcept
{
    for (const auto &tmpl : loglib::BuiltinRegexTemplates())
    {
        if (tmpl.name == name)
        {
            return &tmpl;
        }
    }
    return nullptr;
}
