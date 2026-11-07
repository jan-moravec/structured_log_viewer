#include <loglib/core/bytes_producer.hpp>
#include <loglib/core/file_line_source.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/parse/regex_templates.hpp>
#include <loglib/schema/log_configuration.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>
#include <test_common/log_record.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
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

    /// Maps the file and records its line offsets, as a parse would. Used by
    /// tests that need to pass a `LineSource&` into a `LogLine` ctor. A parse
    /// of such a source asserts, because the offsets are already there; use
    /// the path constructor of `FileLineSource` for that.
    std::unique_ptr<loglib::FileLineSource> CreateFileLineSource() const;

private:
    static constexpr char FILE_PATH[] = "test_file.json";
    std::string mFilePath;
    std::filesystem::path mFsPath;
};

/// The offsets a parser records for text: one past each line feed, and for a last line without a line feed, the text
/// size plus one.
[[nodiscard]] std::vector<std::uint64_t> LineEndOffsets(std::string_view text);

/// A file source over text held in memory, for tests that would otherwise write a temporary file only to parse it. It
/// has no line offsets yet, so a parse fills them in, just as it does for a mapped file.
[[nodiscard]] std::unique_ptr<loglib::FileLineSource> MemorySource(
    std::string text, std::string_view displayName = "memory.log"
);

/// Serves a string through the BytesProducer interface, optionally in chunks of chunkSize bytes, and then reports the
/// end of the stream. WaitForBytes returns at once because the bytes are always there. Tests that need a producer with
/// timing keep their own.
class InMemoryBytesProducer final : public loglib::BytesProducer
{
public:
    explicit InMemoryBytesProducer(std::string bytes, std::size_t chunkSize = std::string::npos)
        : mBytes(std::move(bytes)), mChunkSize(chunkSize)
    {
    }

    std::size_t Read(std::span<char> buffer) override
    {
        const std::scoped_lock lock(mLock);
        if (mStopped)
        {
            return 0;
        }
        const std::size_t give = std::min({mBytes.size() - mCursor, buffer.size(), mChunkSize});
        std::copy_n(mBytes.data() + mCursor, give, buffer.data());
        mCursor += give;
        return give;
    }

    void WaitForBytes(std::chrono::milliseconds /*timeout*/) override
    {
    }

    void Stop() noexcept override
    {
        const std::scoped_lock lock(mLock);
        mStopped = true;
    }

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        const std::scoped_lock lock(mLock);
        return mStopped || mCursor >= mBytes.size();
    }

    [[nodiscard]] std::string DisplayName() const override
    {
        return "in-memory";
    }

private:
    mutable std::mutex mLock;
    std::string mBytes;
    std::size_t mChunkSize;
    std::size_t mCursor = 0;
    bool mStopped = false;
};

void InitializeTimezoneData();

[[nodiscard]] std::filesystem::path FindTestTzdata();

/// Look up a shipped `loglib::RegexTemplate` by exact display name.
/// Small linear scan (~30 entries, only used in test setup).
/// Returns nullptr when @p name matches no built-in so the caller
/// can REQUIRE non-null with a clear error message. Shared between
/// the synthesizer round-trip tests and the per-template
/// `[regex_parser][large]` benchmarks so both look up the same
/// source-of-truth pattern.
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
