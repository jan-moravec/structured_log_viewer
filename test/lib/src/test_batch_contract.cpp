#include "common.hpp"

#include <loglib/core/file_line_source.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_file.hpp>
#include <loglib/core/row_batch.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/log_parser.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/parse/parsers/csv_parser.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/parse/parsers/logfmt_parser.hpp>
#include <loglib/parse/parsers/regex_parser.hpp>
#include <loglib/schema/column_schema.hpp>
#include <loglib/schema/log_configuration.hpp>
#include <loglib/table/log_table.hpp>
#include <loglib/time/timestamp_parsing.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace loglib;

namespace
{

/** Keeps every batch of a parse, in order, with the number of OnStarted and OnFinished calls. */
class RecordingSink final : public LogParseSink
{
public:
    KeyIndex &Keys() override
    {
        return mKeys;
    }

    void OnStarted() override
    {
        ++started;
    }

    void OnBatch(RowBatch batch) override
    {
        batches.push_back(std::move(batch));
    }

    void OnFinished(bool cancelled) override
    {
        ++finished;
        wasCancelled = cancelled;
    }

    [[nodiscard]] std::size_t LastLineNumber() const
    {
        return batches.empty() ? 0 : batches.back().lastLineNumber;
    }

    [[nodiscard]] std::vector<ParseError> Errors() const
    {
        std::vector<ParseError> errors;
        for (const RowBatch &batch : batches)
        {
            errors.insert(errors.end(), batch.errors.begin(), batch.errors.end());
        }
        return errors;
    }

    [[nodiscard]] std::size_t RowCount() const
    {
        std::size_t rows = 0;
        for (const RowBatch &batch : batches)
        {
            rows += batch.lines.size();
        }
        return rows;
    }

    std::vector<RowBatch> batches;
    int started = 0;
    int finished = 0;
    bool wasCancelled = false;

private:
    KeyIndex mKeys;
};

/** Runs a parser over bytes as a file, with a given Stage A batch size, and keeps the sink for inspection. */
void ParseAsFile(
    const LogParser &parser,
    const std::string &fileName,
    const std::string &bytes,
    std::size_t batchSizeBytes,
    RecordingSink &sink,
    unsigned int threads = 0
)
{
    const TestLogFile file(fileName);
    file.Write(bytes);
    auto source = file.CreateFileLineSource();
    ParserOptions options;
    options.batchSizeBytes = batchSizeBytes;
    options.threads = threads;
    parser.ParseStreaming(*source, sink, options);
}

/** Runs a parser over bytes as a stream, delivered in chunks, and keeps the sink for inspection. */
void ParseAsStream(
    const LogParser &parser,
    const std::string &bytes,
    std::size_t chunkSize,
    RecordingSink &sink,
    ParserOptions options = {}
)
{
    StreamLineSource source("memory.log", std::make_unique<InMemoryBytesProducer>(bytes, chunkSize));
    parser.ParseStreaming(source, sink, options);
}

} // namespace

TEST_CASE("The last batch of a parse holds the number of lines read", "[batch_contract][line_count]")
{
    const JsonParser json;
    const LogfmtParser logfmt;

    struct Case
    {
        const char *name;
        const LogParser *parser;
        std::string bytes;
        std::size_t lines;
        std::size_t rows;
    };
    const std::vector<Case> cases = {
        {"three JSON lines", &json, "{\"a\":1}\n{\"a\":2}\n{\"a\":3}\n", 3, 3},
        {"logfmt records over five physical lines",
         &logfmt,
         "level=info msg=first\n  continued line\nlevel=info msg=second\n  more\nlevel=info msg=third\n",
         5,
         3},
        {"no final newline", &json, "{\"a\":1}\n{\"a\":2}", 2, 2},
        {"trailing blank lines", &json, "{\"a\":1}\n\n\n", 3, 1},
        {"a bad line", &json, "{\"a\":1}\nnot json\n{\"a\":3}\n", 3, 2},
        {"empty input", &json, "", 0, 0},
    };

    for (const Case &c : cases)
    {
        SECTION(c.name)
        {
            RecordingSink fileSink;
            ParseAsFile(*c.parser, "batch_contract_lines.log", c.bytes, 0, fileSink);
            CHECK(fileSink.finished == 1);
            CHECK(fileSink.LastLineNumber() == c.lines);
            CHECK(fileSink.RowCount() == c.rows);

            for (const std::size_t chunk : {std::size_t{1}, std::size_t{7}, std::string::npos})
            {
                RecordingSink streamSink;
                ParseAsStream(*c.parser, c.bytes, chunk, streamSink);
                INFO("chunk size " << chunk);
                CHECK(streamSink.finished == 1);
                CHECK(streamSink.LastLineNumber() == c.lines);
                CHECK(streamSink.RowCount() == c.rows);
            }
        }
    }
}

TEST_CASE("A stream counts an oversized line that it drops", "[batch_contract][line_count]")
{
    std::string bytes = "{\"a\":1}\n" + std::string(200, 'x') + "\n{\"a\":3}\n";
    ParserOptions options;
    options.maxLineBytes = 64;
    RecordingSink sink;
    ParseAsStream(JsonParser{}, bytes, 16, sink, options);

    CHECK(sink.LastLineNumber() == 3);
    CHECK(sink.RowCount() == 2);
    const auto errors = sink.Errors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].firstLine == 2);
    CHECK(errors[0].lastLine == 2);
}

TEST_CASE("The line number of every batch never goes backwards", "[batch_contract][line_count]")
{
    std::string bytes;
    for (int i = 0; i < 5000; ++i)
    {
        bytes += "{\"a\":" + std::to_string(i) + "}\n";
    }
    RecordingSink fileSink;
    ParseAsFile(JsonParser{}, "batch_contract_monotonic.jsonl", bytes, 4096, fileSink);
    RecordingSink streamSink;
    ParseAsStream(JsonParser{}, bytes, 1000, streamSink);

    for (const RecordingSink *sink : {&fileSink, &streamSink})
    {
        std::size_t previous = 0;
        for (const RowBatch &batch : sink->batches)
        {
            CHECK(batch.lastLineNumber >= previous);
            previous = batch.lastLineNumber;
        }
        CHECK(sink->LastLineNumber() == 5000);
    }
}

namespace
{

/** Appends every batch to a table, like the CLI does, and remembers whether a batch made the table convert a column. */
class AppendingSink final : public LogParseSink
{
public:
    explicit AppendingSink(LogTable &table)
        : mTable(&table)
    {
    }

    KeyIndex &Keys() override
    {
        return mTable->Keys();
    }

    void OnStarted() override
    {
    }

    void OnBatch(RowBatch batch) override
    {
        const auto result = mTable->AppendBatch(std::move(batch));
        if (result.backfilledColumns.has_value())
        {
            tableConvertedColumns = true;
        }
    }

    void OnFinished(bool /*cancelled*/) override
    {
    }

    bool tableConvertedColumns = false;

private:
    LogTable *mTable;
};

[[nodiscard]] LogConfiguration TimeConfiguration()
{
    LogConfiguration configuration;
    Column ts;
    ts.header = "ts";
    ts.keys = {"ts"};
    ts.type = ColumnType::Time;
    ts.printFormat = "%F %T";
    ts.parseFormats = {"%F %T"};
    configuration.columns.push_back(ts);
    return configuration;
}

constexpr const char *TIME_LINES = "{\"ts\":\"2026-01-15 12:00:00\"}\n{\"ts\":\"2026-01-15 12:00:01\"}\n";

/** Parses TIME_LINES into a table and checks the time cells. Returns whether the table had to convert a column. */
[[nodiscard]] bool ParseTimeLinesIntoTable(bool passTimeColumns)
{
    const TestLogFile file("batch_contract_time.jsonl");
    file.Write(TIME_LINES);
    auto source = file.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.Configuration().SetConfiguration(TimeConfiguration());
    table.BeginStreaming(std::move(source));

    ParserOptions options;
    if (passTimeColumns)
    {
        options.timeColumns = TimeColumnsOf(table.Configuration().Configuration());
    }
    AppendingSink sink(table);
    JsonParser{}.ParseStreaming(*sourcePtr, sink, options);

    REQUIRE(table.RowCount() == 2);
    REQUIRE(table.ColumnCount() >= 1);
    CHECK(table.GetHeader(0) == "ts");
    for (std::size_t row = 0; row < 2; ++row)
    {
        CHECK(std::holds_alternative<TimeStamp>(table.GetValue(row, 0)));
    }
    return sink.tableConvertedColumns;
}

} // namespace

TEST_CASE("Every batch lists the time keys the parser converted", "[batch_contract][time_keys]")
{
    ParserOptions options;
    options.timeColumns = TimeConfiguration().columns;
    Column second;
    second.header = "when";
    second.keys = {"when", "ts"};
    second.type = ColumnType::Time;
    second.parseFormats = {"%F %T"};
    options.timeColumns.push_back(second);

    const std::string bytes = TIME_LINES;
    SECTION("a file")
    {
        RecordingSink sink;
        const TestLogFile file("batch_contract_time_keys.jsonl");
        file.Write(bytes);
        auto source = file.CreateFileLineSource();
        JsonParser{}.ParseStreaming(*source, sink, options);
        REQUIRE_FALSE(sink.batches.empty());
        const KeyId ts = sink.Keys().Find("ts");
        const KeyId when = sink.Keys().Find("when");
        REQUIRE(ts != INVALID_KEY_ID);
        REQUIRE(when != INVALID_KEY_ID);
        std::vector<KeyId> expected = {ts, when};
        std::ranges::sort(expected);
        for (const RowBatch &batch : sink.batches)
        {
            CHECK(batch.convertedTimeKeys == expected);
        }
    }
    SECTION("a stream")
    {
        RecordingSink sink;
        ParseAsStream(JsonParser{}, bytes, 5, sink, options);
        REQUIRE_FALSE(sink.batches.empty());
        std::vector<KeyId> expected = {sink.Keys().Find("ts"), sink.Keys().Find("when")};
        std::ranges::sort(expected);
        for (const RowBatch &batch : sink.batches)
        {
            CHECK(batch.convertedTimeKeys == expected);
        }
    }
    SECTION("an empty input still carries the list")
    {
        RecordingSink sink;
        ParseAsStream(JsonParser{}, "", 5, sink, options);
        REQUIRE(sink.batches.size() == 1);
        CHECK(sink.batches[0].convertedTimeKeys.size() == 2);
    }
    SECTION("no time columns give an empty list")
    {
        RecordingSink sink;
        ParseAsStream(JsonParser{}, bytes, 5, sink);
        for (const RowBatch &batch : sink.batches)
        {
            CHECK(batch.convertedTimeKeys.empty());
        }
    }
}

TEST_CASE(
    "A parse without time columns still gives time cells for a configured Time column", "[batch_contract][time_keys]"
)
{
    CHECK(ParseTimeLinesIntoTable(false));
}

TEST_CASE(
    "A parse with time columns gives the same cells without a table-side back-fill", "[batch_contract][time_keys]"
)
{
    CHECK_FALSE(ParseTimeLinesIntoTable(true));
}
