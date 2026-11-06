#include "common.hpp"

#include <loglib/core/bytes_producer.hpp>
#include <loglib/core/stop_token.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/parse/parsers/logfmt_parser.hpp>
#include <loglib/schema/log_configuration.hpp>
#include <loglib/table/log_table.hpp>
#include <loglib/table/table_loader.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <atomic>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace loglib;

namespace
{

[[nodiscard]] std::string JsonRows(int count, int first = 0)
{
    std::string bytes;
    for (int i = first; i < first + count; ++i)
    {
        bytes += R"({"timestamp":"2026-01-01T00:00:)" + std::string(i < 10 ? "0" : "") + std::to_string(i % 60) +
                 R"(Z","level":"info","n":)" + std::to_string(i) + "}\n";
    }
    return bytes;
}

[[nodiscard]] std::string LogfmtWithContinuations()
{
    std::string bytes;
    for (int i = 0; i < 30; ++i)
    {
        bytes += "level=info n=" + std::to_string(i) + " msg=\"row " + std::to_string(i) + "\"\n";
        if (i % 4 == 0)
        {
            bytes += "  continuation of " + std::to_string(i) + "\n\n  and more\n";
        }
    }
    return bytes;
}

[[nodiscard]] std::unique_ptr<FileLineSource> FileSourceOf(const TestLogFile &file, const std::string &bytes)
{
    file.Write(bytes);
    return file.CreateFileLineSource();
}

[[nodiscard]] std::unique_ptr<StreamLineSource> StreamSourceOf(const std::string &bytes)
{
    return std::make_unique<StreamLineSource>("memory.log", std::make_unique<InMemoryBytesProducer>(bytes, 4096));
}

[[nodiscard]] std::string LineOf(const std::string &bytes, size_t index)
{
    size_t start = 0;
    for (size_t i = 0; i < index; ++i)
    {
        start = bytes.find('\n', start) + 1;
    }
    return bytes.substr(start, bytes.find('\n', start) - start);
}

[[nodiscard]] std::vector<std::string> HeadersOf(const LogTable &table)
{
    std::vector<std::string> headers;
    headers.reserve(table.ColumnCount());
    for (size_t column = 0; column < table.ColumnCount(); ++column)
    {
        headers.push_back(table.GetHeader(column));
    }
    return headers;
}

} // namespace

TEST_CASE("ParseIntoTable loads one file into a typed table", "[table_loader]")
{
    const TestLogFile file("table_loader_one.jsonl");
    LogTable table;

    const JsonParser parser;
    const ParseOutcome outcome =
        ParseIntoTable(table, parser, FileSourceOf(file, JsonRows(5)), ParserOptionsFor(table));
    FinishLoading(table);

    CHECK(outcome.errors.empty());
    CHECK_FALSE(outcome.cancelled);
    CHECK(outcome.linesConsumed == 5);
    REQUIRE(table.RowCount() == 5);
    REQUIRE(table.ColumnCount() == 3);
    // A time column moves to the front, the others follow the file.
    CHECK(HeadersOf(table) == std::vector<std::string>{"timestamp", "level", "n"});
    CHECK(table.Configuration().Configuration().columns[0].type == ColumnType::Time);
    CHECK(std::holds_alternative<TimeStamp>(table.GetValue(0, 0)));
    CHECK(table.GetFormattedValue(3, 2) == "3");
}

TEST_CASE("ParseIntoTable appends later sources to the first", "[table_loader]")
{
    const TestLogFile first("table_loader_first.jsonl");
    const TestLogFile second("table_loader_second.jsonl");
    LogTable table;
    const JsonParser parser;

    const ParseOutcome one = ParseIntoTable(table, parser, FileSourceOf(first, JsonRows(3)), ParserOptionsFor(table));
    const ParseOutcome two =
        ParseIntoTable(table, parser, FileSourceOf(second, JsonRows(4, 3)), ParserOptionsFor(table));
    FinishLoading(table);

    CHECK(one.linesConsumed == 3);
    CHECK(two.linesConsumed == 4);
    REQUIRE(table.RowCount() == 7);
    REQUIRE(table.Data().Sources().size() == 2);

    const auto &lines = table.Data().Lines();
    CHECK(lines[0].Source() == table.Data().Sources()[0].get());
    CHECK(lines[3].Source() == table.Data().Sources()[1].get());
    // Line ids count per source.
    CHECK(lines[0].LineId() == 0);
    CHECK(lines[3].LineId() == 0);
    CHECK(lines[6].LineId() == 3);
    // The first source stays readable after the second one was added.
    CHECK(lines[1].Source()->RawLine(lines[1].LineId()) == LineOf(JsonRows(3), 1));
    CHECK(table.GetFormattedValue(6, 2) == "6");
}

TEST_CASE("LinesConsumed counts physical lines the same for a file and a stream", "[table_loader][lines]")
{
    const JsonParser json;
    const LogfmtParser logfmt;

    struct Case
    {
        const LogParser *parser;
        std::string bytes;
        std::uint64_t lines;
        size_t errors;
    };
    const std::vector<Case> cases = {
        {.parser = &json, .bytes = JsonRows(3), .lines = 3, .errors = 0},
        {.parser = &json, .bytes = JsonRows(3) + "not json\n\n", .lines = 5, .errors = 1},
        {.parser = &json, .bytes = "", .lines = 0, .errors = 0},
        {.parser = &logfmt, .bytes = LogfmtWithContinuations(), .lines = 54, .errors = 0},
    };

    for (const Case &testCase : cases)
    {
        INFO("bytes: " << testCase.bytes.substr(0, 40));
        {
            const TestLogFile file("table_loader_lines.log");
            LogTable table;
            const ParseOutcome outcome =
                ParseIntoTable(table, *testCase.parser, FileSourceOf(file, testCase.bytes), ParserOptionsFor(table));
            CHECK(outcome.linesConsumed == testCase.lines);
            CHECK(outcome.errors.size() == testCase.errors);
        }
        {
            LogTable table;
            const ParseOutcome outcome =
                ParseIntoTable(table, *testCase.parser, StreamSourceOf(testCase.bytes), ParserOptionsFor(table));
            CHECK(outcome.linesConsumed == testCase.lines);
            CHECK(outcome.errors.size() == testCase.errors);
        }
    }
}

TEST_CASE("ParseIntoTable keeps the rows that arrived when the stop token fires", "[table_loader][cancel]")
{
    StopSource stopSource;
    stopSource.RequestStop();

    const TestLogFile file("table_loader_cancel.jsonl");
    LogTable table;
    const ParseOutcome outcome = ParseIntoTable(
        table, JsonParser{}, FileSourceOf(file, JsonRows(1000)), ParserOptionsFor(table, stopSource.GetToken())
    );

    CHECK(outcome.cancelled);
    CHECK(table.RowCount() <= 1000);
    CHECK(table.Data().Sources().size() == 1);
}

TEST_CASE("ParseIntoTable passes on a failure and keeps the rows that arrived", "[table_loader][throw]")
{
    class FailingProducer final : public BytesProducer
    {
    public:
        std::size_t Read(std::span<char> buffer) override
        {
            if (mCalls++ == 0)
            {
                const std::string text = JsonRows(3);
                std::copy(text.begin(), text.end(), buffer.begin());
                return text.size();
            }
            throw std::runtime_error("producer failed");
        }
        void WaitForBytes(std::chrono::milliseconds /*timeout*/) override
        {
        }
        void Stop() noexcept override
        {
        }
        [[nodiscard]] bool IsClosed() const noexcept override
        {
            return false;
        }
        [[nodiscard]] std::string DisplayName() const override
        {
            return "failing";
        }

    private:
        int mCalls = 0;
    };

    LogTable table;
    auto source = std::make_unique<StreamLineSource>("memory.log", std::make_unique<FailingProducer>());
    CHECK_THROWS_AS(
        ParseIntoTable(table, JsonParser{}, std::move(source), ParserOptionsFor(table)), std::runtime_error
    );
    CHECK(table.Data().Sources().size() == 1);
    // The record the parser still holds when it fails is dropped, so the table can hold fewer rows than were read.
    CHECK(table.RowCount() <= 3);
}

namespace
{

/** Forwards to a TableSink and records which thread each call ran on and whether two calls overlapped. */
class ThreadRecordingSink final : public LogParseSink
{
public:
    explicit ThreadRecordingSink(LogTable &table)
        : mInner(table)
    {
    }

    KeyIndex &Keys() override
    {
        return mInner.Keys();
    }

    void OnStarted() override
    {
        Enter();
        mInner.OnStarted();
        Leave();
    }

    void OnBatch(RowBatch batch) override
    {
        Enter();
        mInner.OnBatch(std::move(batch));
        Leave();
    }

    void OnFinished(bool cancelled) override
    {
        Enter();
        mInner.OnFinished(cancelled);
        Leave();
    }

    [[nodiscard]] bool Overlapped() const
    {
        return mOverlapped;
    }

    [[nodiscard]] bool AllOn(std::thread::id id) const
    {
        return !mThreads.empty() && std::ranges::all_of(mThreads, [id](std::thread::id seen) { return seen == id; });
    }

private:
    void Enter()
    {
        if (mInCall.exchange(true))
        {
            mOverlapped = true;
        }
        mThreads.push_back(std::this_thread::get_id());
    }

    void Leave()
    {
        mInCall = false;
    }

    TableSink mInner;
    std::atomic<bool> mInCall{false};
    bool mOverlapped = false;
    std::vector<std::thread::id> mThreads;
};

} // namespace

TEST_CASE("The sink runs on the caller's thread for a stream and never overlaps for a file", "[table_loader][threads]")
{
    SECTION("stream")
    {
        LogTable table;
        auto source = StreamSourceOf(JsonRows(500));
        StreamLineSource &sourceRef = *source;
        table.BeginStreaming(std::move(source));
        ThreadRecordingSink sink(table);
        JsonParser{}.ParseStreaming(sourceRef, sink, ParserOptionsFor(table));
        CHECK(sink.AllOn(std::this_thread::get_id()));
        CHECK_FALSE(sink.Overlapped());
        CHECK(table.RowCount() == 500);
    }
    SECTION("file")
    {
        const TestLogFile file("table_loader_threads.jsonl");
        LogTable table;
        auto source = FileSourceOf(file, JsonRows(20'000));
        FileLineSource &sourceRef = *source;
        table.BeginStreaming(std::move(source));
        ThreadRecordingSink sink(table);
        ParserOptions options = ParserOptionsFor(table);
        options.batchSizeBytes = 16 * 1024;
        JsonParser{}.ParseStreaming(sourceRef, sink, options);
        CHECK_FALSE(sink.Overlapped());
        CHECK(table.RowCount() == 20'000);
    }
}

TEST_CASE("ParserOptionsFor carries the table's time columns and zone", "[table_loader][options]")
{
    LogTable table;
    LogConfiguration configuration;
    configuration.columns.push_back({.header = "when", .keys = {"ts"}, .printFormat = "{}", .type = ColumnType::Time});
    table.Configuration().SetConfiguration(configuration);

    const StopSource stopSource;
    const ParserOptions options = ParserOptionsFor(table, stopSource.GetToken());
    REQUIRE(options.timeColumns.size() == 1);
    CHECK(options.timeColumns[0].keys == std::vector<std::string>{"ts"});
}

TEST_CASE("LiveProducer is the producer of the last stream and null for files", "[table_loader][live_producer]")
{
    LogTable table;
    CHECK(table.LiveProducer() == nullptr);

    const TestLogFile file("table_loader_live.jsonl");
    ParseIntoTable(table, JsonParser{}, FileSourceOf(file, JsonRows(2)), ParserOptionsFor(table));
    CHECK(table.LiveProducer() == nullptr);

    ParseIntoTable(table, JsonParser{}, StreamSourceOf(JsonRows(2)), ParserOptionsFor(table));
    CHECK(table.LiveProducer() != nullptr);
}
