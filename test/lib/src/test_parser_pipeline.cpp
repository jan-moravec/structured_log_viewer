#include "common.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/internal/compact_log_value.hpp>
#include <loglib/internal/static_parser_pipeline.hpp>
#include <loglib/internal/timestamp_promotion.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/stop_token.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using loglib::FileLineSource;
using loglib::KeyIndex;
using loglib::LogConfiguration;
using loglib::LogFile;
using loglib::LogLine;
using loglib::LogParseSink;
using loglib::LogValue;
using loglib::StreamedBatch;

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
        FileLineSource &source,
        LogParseSink &sink,
        const loglib::ParserOptions &options,
        const loglib::internal::AdvancedParserOptions &advanced
    ) const
    {
        FileLineSource *sourcePtr = &source;
        LogFile &file = source.File();
        const char *fileBegin = file.Data();
        const char *fileEnd = (fileBegin != nullptr) ? fileBegin + file.Size() : nullptr;
        const size_t batchBytes = advanced.batchSizeBytes != 0
                                      ? advanced.batchSizeBytes
                                      : loglib::internal::AdvancedParserOptions::DEFAULT_BATCH_SIZE_BYTES;
        const char *cursor = fileBegin;

        auto stageA = [cursor, fileEnd, batchBytes](ByteRange &out) mutable -> bool {
            if (cursor >= fileEnd)
            {
                return false;
            }
            const char *batchBegin = cursor;
            // Size-bounded advance: `cursor + batchBytes` is UB if it lands
            // more than one past `fileEnd`, which the default batch size
            // hits on every sub-batchSize file's final batch.
            const size_t remaining = static_cast<size_t>(fileEnd - cursor);
            const size_t advance = std::min(batchBytes, remaining);
            const char *target = cursor + advance;
            if (advance < remaining)
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

        auto stageB = [sourcePtr, fileBegin](
                          ByteRange token,
                          loglib::internal::WorkerScratch<WorkerState> &worker,
                          KeyIndex &keys,
                          std::span<const loglib::internal::TimeColumnSpec> timeColumns,
                          loglib::internal::ParsedPipelineBatch &parsed
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
                        loglib::internal::ParsedLineError{relativeLineNumber, "injected parser failure"}
                    );
                    ++relativeLineNumber;
                    continue;
                }

                std::vector<std::pair<loglib::KeyId, loglib::internal::CompactLogValue>> values;
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

                    const loglib::KeyId keyId = loglib::internal::InternKeyVia(keyView, keys, &worker.keyCache);

                    // Park the value bytes in the per-batch arena and
                    // record an `OwnedString` compact value pointing at
                    // them. Stage C concatenates this arena onto the
                    // `LogFile` arena and rebases the offsets on the way
                    // out — the inline `worker.PromoteTimestamps` call
                    // below sees the per-batch view.
                    const uint64_t offset = parsed.ownedStringsArena.size();
                    parsed.ownedStringsArena.append(valueView.data(), valueView.size());
                    auto val = loglib::internal::CompactLogValue::MakeOwnedString(
                        offset, static_cast<uint32_t>(valueView.size())
                    );
                    auto it = values.begin();
                    while (it != values.end() && it->first < keyId)
                    {
                        ++it;
                    }
                    if (it != values.end() && it->first == keyId)
                    {
                        it->second = val;
                    }
                    else
                    {
                        values.emplace(it, keyId, val);
                    }
                }

                // 0-based offset within the batch so that, after Stage C
                // shifts every line's id by the running line cursor (via
                // `LogLine::ShiftLineId`), `LogFile::GetLine(N)` round-trips
                // the source bytes for the line.
                LogLine logLine(std::move(values), keys, *sourcePtr, relativeLineNumber - 1);
                parsed.lines.push_back(std::move(logLine));
                worker.PromoteTimestamps(parsed.lines.back(), timeColumns, std::string_view(parsed.ownedStringsArena));

                ++relativeLineNumber;
            }

            parsed.totalLineCount = relativeLineNumber - 1;
        };

        loglib::internal::RunStaticParserPipeline<ByteRange, WorkerState>(
            source, sink, options, advanced, stageA, stageB
        );
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
class CollectingSink : public LogParseSink
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

// Regression for #4: Stage A used to compute `cursor + batchSize` even when
// that pointer landed more than one past `fileEnd`, which is UB. A
// sub-batchSize file (e.g. a single short line) tripped it on every parse.
// The size-bounded advance form keeps the pointer valid; this test exercises
// the smallest possible non-empty file so the only batch the harness emits
// goes through the `advance == remaining` branch.
TEST_CASE("Mock parser: single sub-batchSize file does not overshoot fileEnd", "[mock_parser]")
{
    TempTextFile fixture("level=info msg=tiny\n", "test_kv_tiny.log");
    loglib::ParserOptions options;
    loglib::internal::AdvancedParserOptions advanced;
    advanced.threads = 1;
    advanced.batchSizeBytes = 4 * 1024 * 1024; // larger than the file

    CollectingSink sink;
    KeyValueLineParser parser;
    FileLineSource source(std::make_unique<LogFile>(fixture.Path()));
    parser.ParseStreaming(source, sink, options, advanced);

    REQUIRE(sink.startedCount == 1);
    REQUIRE(sink.finishedCount == 1);
    REQUIRE_FALSE(sink.cancelled);
    REQUIRE(sink.batches.size() == 1);
    REQUIRE(sink.batches.front().lines.size() == 1);
}

// Regression for #5: the sink contract states the harness always emits at
// least one `OnBatch` (possibly empty) before `OnFinished`. The early-exit
// paths (empty file / `stop_requested` before Stage A starts) used to skip
// `OnBatch` entirely. The two sub-cases below exercise both paths.
TEST_CASE("Mock parser: empty and early-stopped parses still emit one OnBatch", "[mock_parser]")
{
    SECTION("Empty file")
    {
        TempTextFile fixture(std::string{}, "test_kv_empty.log");
        loglib::ParserOptions options;
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;

        CollectingSink sink;
        KeyValueLineParser parser;
        FileLineSource source(std::make_unique<LogFile>(fixture.Path()));
        parser.ParseStreaming(source, sink, options, advanced);

        CHECK(sink.startedCount == 1);
        CHECK(sink.finishedCount == 1);
        CHECK_FALSE(sink.cancelled);
        REQUIRE(sink.batches.size() == 1);
        CHECK(sink.batches.front().lines.empty());
        CHECK(sink.batches.front().firstLineNumber == 1);
    }

    SECTION("Stop requested before parse starts")
    {
        TempTextFile fixture("level=info msg=hi\n", "test_kv_stop_before.log");
        loglib::StopSource stopSource;
        stopSource.request_stop();
        loglib::ParserOptions options;
        options.stopToken = stopSource.get_token();
        loglib::internal::AdvancedParserOptions advanced;
        advanced.threads = 1;

        CollectingSink sink;
        KeyValueLineParser parser;
        FileLineSource source(std::make_unique<LogFile>(fixture.Path()));
        parser.ParseStreaming(source, sink, options, advanced);

        CHECK(sink.startedCount == 1);
        CHECK(sink.finishedCount == 1);
        CHECK(sink.cancelled);
        REQUIRE(sink.batches.size() == 1);
        CHECK(sink.batches.front().lines.empty());
        CHECK(sink.batches.front().firstLineNumber == 1);
    }
}

// Regression for #6: `firstLineNumber` must defer its prime until the
// first non-empty `parsed.lines`, so it never overshoots `lines.front()`.
TEST_CASE("Mock parser: firstLineNumber matches the first line in the batch", "[mock_parser]")
{
    // 6 errors then 6 records. `batchSizeBytes = errorBlockBytes - 1` makes
    // Stage A break at the last newline of the error block so the all-error
    // chunk ships first.
    std::string content;
    for (int i = 0; i < 6; ++i)
    {
        content += "!err" + std::to_string(i) + "\n";
    }
    const size_t errorBlockBytes = content.size();
    for (int i = 0; i < 6; ++i)
    {
        content += "level=info index=" + std::to_string(i) + "\n";
    }

    TempTextFile fixture(content, "test_kv_empty_primed.log");
    loglib::ParserOptions options;
    loglib::internal::AdvancedParserOptions advanced;
    advanced.threads = 1;
    advanced.batchSizeBytes = errorBlockBytes - 1;

    CollectingSink sink;
    KeyValueLineParser parser;
    FileLineSource source(std::make_unique<LogFile>(fixture.Path()));
    parser.ParseStreaming(source, sink, options, advanced);

    REQUIRE_FALSE(sink.cancelled);
    REQUIRE(sink.batches.size() >= 1);
    for (const auto &b : sink.batches)
    {
        if (!b.lines.empty())
        {
            const size_t firstObservedLine = b.lines.front().LineId();
            CAPTURE(b.firstLineNumber, firstObservedLine);
            // For this fixture (errors-only chunk then records chunk),
            // the prime fires on the records chunk so the values match
            // exactly. The +1 reconciles 1-based vs 0-based numbering.
            CHECK(b.firstLineNumber == firstObservedLine + 1);
        }
    }
}

TEST_CASE("Mock parser: multi-batch parse emits LogLines and newKeys", "[mock_parser]")
{
    constexpr size_t RECORD_COUNT = 5'000;
    TempTextFile fixture(GenerateRecords(RECORD_COUNT));
    loglib::ParserOptions options;
    loglib::internal::AdvancedParserOptions advanced;
    advanced.batchSizeBytes = 8 * 1024;
    advanced.threads = 2;

    CollectingSink sink;
    KeyValueLineParser parser;
    FileLineSource source(std::make_unique<LogFile>(fixture.Path()));
    parser.ParseStreaming(source, sink, options, advanced);

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
    REQUIRE(totalLines == RECORD_COUNT);

    std::sort(allNewKeys.begin(), allNewKeys.end());
    REQUIRE(allNewKeys == std::vector<std::string>{"index", "level", "msg"});

    LogValue lastIndex = sink.batches.back().lines.back().GetValue("index");
    REQUIRE(std::holds_alternative<std::string>(lastIndex));
    REQUIRE(std::get<std::string>(lastIndex) == std::to_string(RECORD_COUNT - 1));
}

TEST_CASE("Mock parser: per-line errors propagate through StreamedBatch::errors", "[mock_parser]")
{
    std::string content = "level=info msg=ok\n";
    content += "!boom\n";
    content += "level=warn msg=ok2\n";
    content += "!second_error\n";

    TempTextFile fixture(content);
    loglib::ParserOptions options;
    loglib::internal::AdvancedParserOptions advanced;
    advanced.threads = 1;
    advanced.batchSizeBytes = 1024 * 1024;

    CollectingSink sink;
    KeyValueLineParser parser;
    FileLineSource source(std::make_unique<LogFile>(fixture.Path()));
    parser.ParseStreaming(source, sink, options, advanced);

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

// Regression: when errors land past the first Stage A batch, Stage C must
// translate the per-batch-relative line number Stage B emits into the
// absolute (file-wide) line number before the error string reaches the sink.
TEST_CASE(
    "Mock parser: error line numbers stay absolute across multiple pipeline batches",
    "[mock_parser][error_line_numbers]"
)
{
    // Build a fixture where every 100th line is malformed, large enough that
    // a tiny `batchSizeBytes` forces several Stage A batches.
    std::string content;
    constexpr size_t TOTAL_LINES = 600;
    constexpr size_t ERROR_EVERY = 100;
    std::vector<size_t> expectedErrorLines;
    for (size_t i = 1; i <= TOTAL_LINES; ++i)
    {
        if (i % ERROR_EVERY == 0)
        {
            content += "!boom_" + std::to_string(i) + "\n";
            expectedErrorLines.push_back(i);
        }
        else
        {
            content += "index=" + std::to_string(i) + "\n";
        }
    }

    TempTextFile fixture(content, "test_kv_multi_batch_errors.log");
    loglib::ParserOptions options;
    loglib::internal::AdvancedParserOptions advanced;
    advanced.threads = 1;
    advanced.batchSizeBytes = 4 * 1024;

    CollectingSink sink;
    KeyValueLineParser parser;
    FileLineSource source(std::make_unique<LogFile>(fixture.Path()));
    parser.ParseStreaming(source, sink, options, advanced);

    REQUIRE_FALSE(sink.cancelled);
    REQUIRE(sink.batches.size() >= 1);

    std::vector<std::string> allErrors;
    for (const auto &b : sink.batches)
    {
        for (const auto &e : b.errors)
        {
            allErrors.push_back(e);
        }
    }
    REQUIRE(allErrors.size() == expectedErrorLines.size());
    for (size_t i = 0; i < expectedErrorLines.size(); ++i)
    {
        const std::string expected = "Error on line " + std::to_string(expectedErrorLines[i]);
        INFO("error #" << i << " text: " << allErrors[i] << ", expected to start with: " << expected);
        CHECK(allErrors[i].find(expected) != std::string::npos);
    }
}

TEST_CASE("Mock parser: cancellation latency bounded by ntokens x batch size", "[mock_parser][cancellation]")
{
    constexpr size_t RECORD_COUNT = 200'000;
    TempTextFile fixture(GenerateRecords(RECORD_COUNT));
    constexpr size_t BATCH_BYTES = 64 * 1024;
    constexpr unsigned int THREADS = 4;

    loglib::ParserOptions options;
    loglib::internal::AdvancedParserOptions advanced;
    advanced.batchSizeBytes = BATCH_BYTES;
    advanced.threads = THREADS;

    struct CancellingSink : CollectingSink
    {
        loglib::StopSource stop;
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
    FileLineSource source(std::make_unique<LogFile>(fixture.Path()));
    parser.ParseStreaming(source, sink, options, advanced);

    REQUIRE(sink.cancelled);
    const auto latency = sink.finishedAt - sink.requestedAt;
    const auto latencyMs = std::chrono::duration<double, std::milli>(latency).count();

    // The pipeline harness picks `ntokens = 2 * effectiveThreads` by default,
    // so the upper bound on the cancellation latency is roughly
    // `2 * THREADS * BATCH_BYTES` worth of pending work to drain.
    INFO(
        "Cancellation latency = " << latencyMs << " ms (threads=" << THREADS << ", batch=" << BATCH_BYTES << " bytes)"
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
    FileLineSource source(std::make_unique<LogFile>(fixture.Path()));
    parser.ParseStreaming(source, sink, options, advanced);

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
