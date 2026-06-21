#include <loglib/file_line_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_line.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>
#include <test_common/log_record.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Streaming-fixture spec: generate and write `count` random records on the
// fly (no in-memory record vector), seeded for reproducibility. Used by the
// large benchmark to avoid materializing millions of `LogRecord`s.
//
// `timestamps` controls how the `timestamp` field is stamped on each record;
// see `test_common::TimestampPolicy`. Default = wall clock per record. Pass
// a fixed `baseTime` (e.g. via `bench::DeterministicBenchmarkTimestamps()`)
// to make fixtures byte-identical across runs and across formats.
struct StreamedRecords
{
    std::size_t count = 0;
    std::uint32_t seed = test_common::MakeRandomSeed();
    test_common::TimestampPolicy timestamps{};
};

// RAII fixture that serializes `LogRecord`s to disk through a `LogFormat`,
// cleaning the file up on destruction. Two construction paths:
//   * materialized — keeps the records so tests can assert on them;
//   * streaming    — generates records on the fly, keeping only the count.
//
// When `filePath` is left empty (the default), the on-disk name is derived
// from the format's `suggestedExtension` (`test.jsonl`, `test.logfmt`, …)
// so simultaneous fixtures of different formats do not collide on a single
// shared file name.
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
    // Only populated on the materialized path; empty for streaming fixtures.
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
