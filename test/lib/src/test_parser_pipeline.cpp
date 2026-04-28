#include "common.hpp"

#include "parser_pipeline.hpp"
#include "timestamp_promotion.hpp"

#include <loglib/internal/parser_options.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/streaming_log_sink.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using loglib::KeyIndex;
using loglib::LogConfiguration;
using loglib::LogFile;
using loglib::LogFileReference;
using loglib::LogLine;
using loglib::LogValue;
using loglib::StreamedBatch;
using loglib::StreamingLogSink;

namespace
{

/// Mock text-format parser used to exercise the shared streaming pipeline harness
/// independently of `JsonParser`. Format: one record per line, fields separated by a
/// single space, each field of the form `key=value`. A line beginning with `!` is
/// treated as a parse error so the harness's per-line error propagation can be
/// covered without inventing a syntax-error fixture.
class KeyValueLineParser
{
public:
    struct ByteRange
    {
        const char *bytesBegin = nullptr;
        const char *bytesEnd = nullptr;
    };

    struct WorkerState
    {
    };

    void ParseStreaming(
        LogFile &file,
        StreamingLogSink &sink,
        const loglib::ParserOptions &options,
        const loglib::internal::AdvancedParserOptions &advanced
    ) const
    {
        LogFile *filePtr = &file;
        const char *fileBegin = file.Data();
        const char *fileEnd = (fileBegin != nullptr) ? fileBegin + file.Size() : nullptr;
        const size_t batchBytes = advanced.batchSizeBytes != 0
                                      ? advanced.batchSizeBytes
                                      : loglib::internal::AdvancedParserOptions::kDefaultBatchSizeBytes;
        const char *cursor = fileBegin;

        auto stageA = [cursor, fileEnd, batchBytes](ByteRange &out) mutable -> bool {
            if (cursor >= fileEnd)
            {
                return false;
            }
            const char *batchBegin = cursor;
            const char *target = std::min(cursor + batchBytes, fileEnd);
            if (target < fileEnd)
            {
                const char *newline =
                    static_cast<const char *>(memchr(target, '\n', static_cast<size_t>(fileEnd - target)));
                cursor = (newline != nullptr) ? newline + 1 : fileEnd;
            }
            else
            {
                cursor = fileEnd;
            }
            out.bytesBegin = batchBegin;
            out.bytesEnd = cursor;
            return true;
        };

        auto stageB = [filePtr, fileBegin](
                          ByteRange token,
                          loglib::detail::WorkerScratch<WorkerState> &worker,
                          KeyIndex &keys,
                          std::span<const loglib::detail::TimeColumnSpec> timeColumns,
                          loglib::detail::ParsedPipelineBatch &parsed
                      ) {
            const char *cur = token.bytesBegin;
            const char *end = token.bytesEnd;
            size_t relativeLineNumber = 1;

            while (cur < end)
            {
                const char *lineStart = cur;
                const char *newline = static_cast<const char *>(memchr(cur, '\n', static_cast<size_t>(end - cur)));
                const char *lineEnd = (newline != nullptr) ? newline : end;
                cur = (newline != nullptr) ? newline + 1 : end;

                // Match `LogFile::GetLine`'s `stopOffset - startOffset - 1` length
                // formula: when the last line of the file is unterminated, push
                // `fileSize + 1` so the final character isn't trimmed off.
                const uint64_t nextOffset = static_cast<uint64_t>(cur - fileBegin) + (newline == nullptr ? 1u : 0u);
                parsed.localLineOffsets.push_back(nextOffset);

                std::string_view line(lineStart, static_cast<size_t>(lineEnd - lineStart));
                if (!line.empty() && line.back() == '\r')
                {
                    line.remove_suffix(1);
                }

                if (line.empty())
                {
                    ++relativeLineNumber;
                    continue;
                }

                if (line.front() == '!')
                {
                    parsed.errors.push_back(
                        "Error on line " + std::to_string(relativeLineNumber) + ": injected parser failure"
                    );
                    ++relativeLineNumber;
                    continue;
                }

                std::vector<std::pair<loglib::KeyId, LogValue>> values;
                values.reserve(8);
                size_t pos = 0;
                while (pos < line.size())
                {
                    while (pos < line.size() && line[pos] == ' ')
                    {
                        ++pos;
                    }
                    if (pos >= line.size())
                    {
                        break;
                    }
                    const size_t fieldStart = pos;
                    while (pos < line.size() && line[pos] != ' ')
                    {
                        ++pos;
                    }
                    std::string_view field = line.substr(fieldStart, pos - fieldStart);
                    const size_t eq = field.find('=');
                    if (eq == std::string_view::npos)
                    {
                        continue;
                    }
                    std::string_view keyView = field.substr(0, eq);
                    std::string_view valueView = field.substr(eq + 1);

                    const loglib::KeyId keyId = loglib::detail::InternKeyVia(keyView, keys, &worker.keyCache, true);
                    LogValue val{std::string(valueView)};
                    auto it = values.begin();
                    while (it != values.end() && it->first < keyId)
                    {
                        ++it;
                    }
                    if (it != values.end() && it->first == keyId)
                    {
                        it->second = std::move(val);
                    }
                    else
                    {
                        values.emplace(it, keyId, std::move(val));
                    }
                }

                LogFileReference fileRef(*filePtr, 0);
                LogLine logLine(std::move(values), keys, std::move(fileRef));
                // 0-based offset within the batch so that, after Stage C
                // shifts by the running cursor, `LogFileReference::GetLine`
                // (which calls `LogFile::GetLine(N)` with `N` 0-based) can
                // round-trip the source bytes for the line.
                logLine.FileReference().SetLineNumber(relativeLineNumber - 1);
                parsed.lines.push_back(std::move(logLine));
                worker.PromoteTimestamps(parsed.lines.back(), timeColumns);

                ++relativeLineNumber;
            }

            parsed.totalLineCount = relativeLineNumber - 1;
        };

        loglib::detail::RunParserPipeline<ByteRange, WorkerState>(file, sink, options, advanced, stageA, stageB);
    }
};

/// Test fixture: writes the given lines to disk so they can be mmap'd by `LogFile`.
class TempTextFile
{
public:
    explicit TempTextFile(const std::string &content, std::string filePath = "test_kv.log")
        : mFilePath(std::move(filePath))
    {
        std::ofstream file(mFilePath, std::ios::binary);
        REQUIRE(file.is_open());
        file.write(content.data(), static_cast<std::streamsize>(content.size()));
    }

    ~TempTextFile()
    {
        std::filesystem::remove(mFilePath);
    }

    const std::string &Path() const
    {
        return mFilePath;
    }

private:
    std::string mFilePath;
};

/// Test sink: gathers every batch the harness emits so cases can assert on the
/// aggregate parsed output, the new-keys diff, and the cancellation flag.
class CollectingSink : public StreamingLogSink
{
public:
    KeyIndex &Keys() override
    {
        return mKeys;
    }
    void OnStarted() override
    {
        ++startedCount;
    }
    void OnBatch(StreamedBatch batch) override
    {
        batches.push_back(std::move(batch));
    }
    void OnFinished(bool wasCancelled) override
    {
        cancelled = wasCancelled;
        ++finishedCount;
    }

    KeyIndex mKeys;
    std::vector<StreamedBatch> batches;
    bool cancelled = false;
    int startedCount = 0;
    int finishedCount = 0;
};

std::string GenerateRecords(size_t count)
{
    std::string content;
    content.reserve(count * 64);
    for (size_t i = 0; i < count; ++i)
    {
        content += "level=info index=" + std::to_string(i) + " msg=hello\n";
    }
    return content;
}

} // namespace

TEST_CASE("Mock parser: multi-batch parse emits LogLines and newKeys", "[mock_parser]")
{
    constexpr size_t kRecordCount = 5'000;
    TempTextFile fixture(GenerateRecords(kRecordCount));
    LogFile logFile(fixture.Path());

    loglib::ParserOptions options;
    loglib::internal::AdvancedParserOptions advanced;
    advanced.batchSizeBytes = 8 * 1024;
    advanced.threads = 2;

    CollectingSink sink;
    KeyValueLineParser parser;
    parser.ParseStreaming(logFile, sink, options, advanced);

    REQUIRE(sink.startedCount == 1);
    REQUIRE(sink.finishedCount == 1);
    REQUIRE_FALSE(sink.cancelled);
    REQUIRE(sink.batches.size() >= 1);

    size_t totalLines = 0;
    std::vector<std::string> allNewKeys;
    for (const auto &b : sink.batches)
    {
        totalLines += b.lines.size();
        for (const auto &k : b.newKeys)
        {
            allNewKeys.push_back(k);
        }
    }
    REQUIRE(totalLines == kRecordCount);

    std::sort(allNewKeys.begin(), allNewKeys.end());
    REQUIRE(allNewKeys == std::vector<std::string>{"index", "level", "msg"});

    LogValue lastIndex = sink.batches.back().lines.back().GetValue("index");
    REQUIRE(std::holds_alternative<std::string>(lastIndex));
    REQUIRE(std::get<std::string>(lastIndex) == std::to_string(kRecordCount - 1));
}

TEST_CASE("Mock parser: per-line errors propagate through StreamedBatch::errors", "[mock_parser]")
{
    std::string content = "level=info msg=ok\n";
    content += "!boom\n";
    content += "level=warn msg=ok2\n";
    content += "!second_error\n";

    TempTextFile fixture(content);
    LogFile logFile(fixture.Path());

    loglib::ParserOptions options;
    loglib::internal::AdvancedParserOptions advanced;
    advanced.threads = 1;
    advanced.batchSizeBytes = 1024 * 1024;

    CollectingSink sink;
    KeyValueLineParser parser;
    parser.ParseStreaming(logFile, sink, options, advanced);

    REQUIRE_FALSE(sink.cancelled);

    size_t totalLines = 0;
    std::vector<std::string> allErrors;
    for (const auto &b : sink.batches)
    {
        totalLines += b.lines.size();
        for (const auto &e : b.errors)
        {
            allErrors.push_back(e);
        }
    }
    REQUIRE(totalLines == 2);
    REQUIRE(allErrors.size() == 2);
    REQUIRE(allErrors[0].find("Error on line 2") != std::string::npos);
    REQUIRE(allErrors[1].find("Error on line 4") != std::string::npos);
}

TEST_CASE("Mock parser: cancellation latency bounded by ntokens x batch size", "[mock_parser][cancellation]")
{
    constexpr size_t kRecordCount = 200'000;
    TempTextFile fixture(GenerateRecords(kRecordCount));
    LogFile logFile(fixture.Path());

    constexpr size_t kBatchBytes = 64 * 1024;
    constexpr size_t kNtokens = 4;

    loglib::ParserOptions options;
    loglib::internal::AdvancedParserOptions advanced;
    advanced.batchSizeBytes = kBatchBytes;
    advanced.ntokens = kNtokens;
    advanced.threads = 4;

    struct CancellingSink : CollectingSink
    {
        std::stop_source stop;
        std::chrono::steady_clock::time_point requestedAt{};
        std::chrono::steady_clock::time_point finishedAt{};
        size_t batchCount = 0;

        void OnBatch(StreamedBatch batch) override
        {
            CollectingSink::OnBatch(std::move(batch));
            ++batchCount;
            if (batchCount == 1 && !stop.stop_requested())
            {
                requestedAt = std::chrono::steady_clock::now();
                stop.request_stop();
            }
        }
        void OnFinished(bool wasCancelled) override
        {
            finishedAt = std::chrono::steady_clock::now();
            CollectingSink::OnFinished(wasCancelled);
        }
    };

    CancellingSink sink;
    options.stopToken = sink.stop.get_token();

    KeyValueLineParser parser;
    parser.ParseStreaming(logFile, sink, options, advanced);

    REQUIRE(sink.cancelled);
    const auto latency = sink.finishedAt - sink.requestedAt;
    const auto latencyMs = std::chrono::duration<double, std::milli>(latency).count();

    INFO(
        "Cancellation latency = " << latencyMs << " ms (ntokens=" << kNtokens << ", batch=" << kBatchBytes << " bytes)"
    );

    REQUIRE(latency >= std::chrono::nanoseconds{0});
    REQUIRE(latencyMs < 2000.0);
}

TEST_CASE("Mock parser: timestamp promotion via shared post-decoding hook", "[mock_parser][timestamp]")
{
    InitializeTimezoneData();

    std::string content;
    content += "ts=2024-01-15T10:00:00Z level=info msg=first\n";
    content += "ts=2024-01-15T10:00:01Z level=info msg=second\n";
    content += "ts=2024-01-15T10:00:02Z level=warn msg=third\n";
    TempTextFile fixture(content);
    LogFile logFile(fixture.Path());

    auto configuration = std::make_shared<LogConfiguration>();
    LogConfiguration::Column timeColumn;
    timeColumn.header = "Timestamp";
    timeColumn.keys = {"ts"};
    timeColumn.type = LogConfiguration::Type::time;
    timeColumn.parseFormats = {"%FT%TZ"};
    configuration->columns.push_back(timeColumn);

    loglib::ParserOptions options;
    options.configuration = configuration;
    loglib::internal::AdvancedParserOptions advanced;
    advanced.threads = 1;

    CollectingSink sink;
    KeyValueLineParser parser;
    parser.ParseStreaming(logFile, sink, options, advanced);

    size_t promoted = 0;
    size_t totalLines = 0;
    for (const auto &b : sink.batches)
    {
        for (const auto &line : b.lines)
        {
            ++totalLines;
            LogValue v = line.GetValue("ts");
            if (std::holds_alternative<loglib::TimeStamp>(v))
            {
                ++promoted;
            }
        }
    }
    REQUIRE(totalLines == 3);
    REQUIRE(promoted == 3);
}
