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
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
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

    // The rows point into the source, which points into the file, so they live as long as the sink and are declared
    // first. The batches are destroyed before them.
    std::unique_ptr<TestLogFile> file;
    std::unique_ptr<FileLineSource> fileSource;
    std::unique_ptr<StreamLineSource> streamSource;
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
    // Every call gets its own file, because the sinks of earlier calls may still map theirs.
    static std::atomic<unsigned> counter{0};
    sink.file = std::make_unique<TestLogFile>(std::to_string(counter.fetch_add(1)) + "_" + fileName);
    sink.file->Write(bytes);
    sink.fileSource = sink.file->CreateFileLineSource();
    ParserOptions options;
    options.batchSizeBytes = batchSizeBytes;
    options.threads = threads;
    parser.ParseStreaming(*sink.fileSource, sink, options);
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
    sink.streamSource =
        std::make_unique<StreamLineSource>("memory.log", std::make_unique<InMemoryBytesProducer>(bytes, chunkSize));
    parser.ParseStreaming(*sink.streamSource, sink, options);
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

namespace
{

/** Writes a value with its type, so that Int64 5 and Double 5 differ. */
[[nodiscard]] std::string DescribeValue(const LogValue &value)
{
    struct Visitor
    {
        std::string operator()(std::string_view v) const
        {
            return "s:" + std::string(v);
        }
        std::string operator()(const std::string &v) const
        {
            return "s:" + v;
        }
        std::string operator()(int64_t v) const
        {
            return "i:" + std::to_string(v);
        }
        std::string operator()(uint64_t v) const
        {
            return "u:" + std::to_string(v);
        }
        std::string operator()(double v) const
        {
            return "d:" + std::to_string(v);
        }
        std::string operator()(bool v) const
        {
            return v ? "b:true" : "b:false";
        }
        std::string operator()(TimeStamp v) const
        {
            return "t:" + std::to_string(v.time_since_epoch().count());
        }
        std::string operator()(std::monostate) const
        {
            return "null";
        }
    };
    return std::visit(Visitor{}, value);
}

/** One text per row: every key of the parse in key text order, with the value and its type. */
[[nodiscard]] std::vector<std::string> DescribeRows(RecordingSink &sink)
{
    KeyIndex &keys = sink.Keys();
    std::vector<std::pair<std::string, KeyId>> ordered;
    for (const std::string &key : keys.SortedKeys())
    {
        ordered.emplace_back(key, keys.Find(key));
    }
    std::vector<std::string> rows;
    for (const RowBatch &batch : sink.batches)
    {
        for (const LogLine &line : batch.lines)
        {
            std::string text;
            for (const auto &[key, id] : ordered)
            {
                const LogValue value = line.GetValue(id);
                if (!std::holds_alternative<std::monostate>(value))
                {
                    text += key + "=" + DescribeValue(value) + ";";
                }
            }
            rows.push_back(std::move(text));
        }
    }
    return rows;
}

[[nodiscard]] std::vector<std::string> NewKeysOf(const RecordingSink &sink)
{
    std::vector<std::string> keys;
    for (const RowBatch &batch : sink.batches)
    {
        keys.insert(keys.end(), batch.newKeys.begin(), batch.newKeys.end());
    }
    return keys;
}

/** The facts of a parse that must not depend on how the bytes arrived. */
struct Observed
{
    std::size_t lastLine = 0;
    std::vector<std::string> errors;
    std::vector<std::string> rows;
    std::vector<std::string> newKeys;
};

void CheckSame(const Observed &actual, const Observed &expected)
{
    CHECK(actual.lastLine == expected.lastLine);
    CHECK(actual.errors == expected.errors);
    CHECK(actual.rows == expected.rows);
    CHECK(actual.newKeys == expected.newKeys);
}

[[nodiscard]] Observed Observe(RecordingSink &sink)
{
    std::vector<std::string> errors;
    for (const ParseError &error : sink.Errors())
    {
        errors.push_back(std::to_string(error.firstLine) + "-" + std::to_string(error.lastLine) + ": " + error.message);
    }
    return Observed{sink.LastLineNumber(), std::move(errors), DescribeRows(sink), NewKeysOf(sink)};
}

/** Parses the bytes as a file for each batch size and as a stream for each chunk size, and compares them all. */
void CheckStaticAndStreamAgree(const LogParser &parser, const std::string &bytes, const std::string &fileName)
{
    RecordingSink reference;
    ParseAsFile(parser, fileName, bytes, 4096, reference);
    const Observed expected = Observe(reference);
    CHECK(reference.finished == 1);
    CHECK(expected.rows.size() > 10);

    for (const std::size_t batchSize : {std::size_t{64}, std::size_t{257}})
    {
        RecordingSink sink;
        ParseAsFile(parser, fileName, bytes, batchSize, sink);
        INFO("file, batch size " << batchSize);
        CheckSame(Observe(sink), expected);
    }
    for (const std::size_t chunk : {std::size_t{1}, std::size_t{64}, std::size_t{257}, std::size_t{4096}})
    {
        RecordingSink sink;
        ParseAsStream(parser, bytes, chunk, sink);
        INFO("stream, chunk size " << chunk);
        CheckSame(Observe(sink), expected);
    }
}

[[nodiscard]] std::string JsonInput()
{
    std::string bytes;
    for (int i = 0; i < 60; ++i)
    {
        bytes += "{\"id\":" + std::to_string(i) + ",\"msg\":\"line " + std::to_string(i) + "\"";
        if (i % 7 == 0)
        {
            bytes += ",\"extra" + std::to_string(i) + "\":true";
        }
        if (i % 11 == 0)
        {
            bytes += ",\"esc\\u0041\":1.5";
        }
        bytes += "}\n";
        if (i == 20)
        {
            bytes += "not json at all\n\n";
        }
    }
    return bytes;
}

[[nodiscard]] std::string LogfmtInput()
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

[[nodiscard]] std::string CsvInput()
{
    std::string bytes = "\nid,name,empty\n";
    for (int i = 0; i < 40; ++i)
    {
        bytes += std::to_string(i) + ",name" + std::to_string(i) + ",\n";
        if (i == 10)
        {
            bytes += "1,2,3,4,5\n";
        }
        if (i == 20)
        {
            bytes += "9,\"unterminated\n";
        }
    }
    return bytes;
}

[[nodiscard]] std::string RegexInput()
{
    std::string bytes;
    for (int i = 0; i < 30; ++i)
    {
        bytes += std::to_string(1000 + i) + " INFO message " + std::to_string(i) + "\n";
        if (i % 5 == 0)
        {
            bytes += "    at frame one\n    at frame two\n";
        }
    }
    return bytes;
}

} // namespace

TEST_CASE("The same bytes as a file and as a stream give the same batches", "[batch_contract][equivalence]")
{
    SECTION("json")
    {
        CheckStaticAndStreamAgree(JsonParser{}, JsonInput(), "batch_contract_eq.jsonl");
    }
    SECTION("logfmt with continuations")
    {
        CheckStaticAndStreamAgree(LogfmtParser{}, LogfmtInput(), "batch_contract_eq.log");
    }
    SECTION("csv")
    {
        CheckStaticAndStreamAgree(CsvParser{}, CsvInput(), "batch_contract_eq.csv");
    }
    SECTION("multi-line regex")
    {
        const RegexParser parser(R"(^(?<ts>\d+) (?<level>[A-Z]+) (?<msg>.*)$)", ContinuationMode::UntilNextHeader);
        CheckStaticAndStreamAgree(parser, RegexInput(), "batch_contract_eq_regex.log");
    }
}

TEST_CASE("Declared keys are announced before the keys of rows", "[batch_contract][declared_keys]")
{
    SECTION("a CSV column that is empty in every row still appears")
    {
        const std::string bytes = "a,b,c\n1,2,\n3,4,\n";
        RecordingSink fileSink;
        ParseAsFile(CsvParser{}, "batch_contract_declared.csv", bytes, 0, fileSink);
        RecordingSink streamSink;
        ParseAsStream(CsvParser{}, bytes, 3, streamSink);
        const std::vector<std::string> expected = {"a", "b", "c"};
        CHECK(NewKeysOf(fileSink) == expected);
        CHECK(NewKeysOf(streamSink) == expected);
    }
    SECTION("a regex group that never matches still appears")
    {
        const RegexParser parser(R"(^(?<a>\d+)(?: (?<b>x))?$)");
        const std::string bytes = "1\n2\n";
        RecordingSink fileSink;
        ParseAsFile(parser, "batch_contract_declared.log", bytes, 0, fileSink);
        RecordingSink streamSink;
        ParseAsStream(parser, bytes, 3, streamSink);
        for (const RecordingSink *sink : {&fileSink, &streamSink})
        {
            const auto keys = NewKeysOf(*sink);
            CHECK(std::ranges::find(keys, "a") != keys.end());
            CHECK(std::ranges::find(keys, "b") != keys.end());
            CHECK(sink->RowCount() == 2);
        }
    }
    SECTION("a key that only appears in rows is announced once, in input order")
    {
        const std::string bytes = "{\"z\":1}\n{\"a\":1}\n{\"z\":2,\"m\":1}\n{\"a\":3,\"b\":1}\n";
        for (const std::size_t batchSize : {std::size_t{8}, std::size_t{0}})
        {
            RecordingSink sink;
            ParseAsFile(JsonParser{}, "batch_contract_order.jsonl", bytes, batchSize, sink, 4);
            const std::vector<std::string> expected = {"z", "a", "m", "b"};
            CHECK(NewKeysOf(sink) == expected);
        }
    }
}
