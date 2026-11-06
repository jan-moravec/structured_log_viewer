#include "common.hpp"

#include <loglib/core/file_line_source.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/row_batch.hpp>
#include <loglib/core/stop_token.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/output/exports/export_sink.hpp>
#include <loglib/output/exports/row_exporter.hpp>
#include <loglib/parse/internal/line_decoder.hpp>
#include <loglib/parse/internal/static_parser_pipeline.hpp>
#include <loglib/parse/internal/streaming_parse_loop.hpp>
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
#include <numeric>
#include <span>
#include <stdexcept>
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

    // The rows point into the source, so it lives as long as the sink and is declared first. The batches are destroyed
    // before it.
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
    sink.fileSource = MemorySource(bytes, fileName);
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
    const ParserOptions &options = {}
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
        {
            .name = "three JSON lines",
            .parser = &json,
            .bytes = "{\"a\":1}\n{\"a\":2}\n{\"a\":3}\n",
            .lines = 3,
            .rows = 3,
        },
        {
            .name = "logfmt records over five physical lines",
            .parser = &logfmt,
            .bytes = "level=info msg=first\n  continued line\nlevel=info msg=second\n  more\nlevel=info msg=third\n",
            .lines = 5,
            .rows = 3,
        },
        {.name = "no final newline", .parser = &json, .bytes = "{\"a\":1}\n{\"a\":2}", .lines = 2, .rows = 2},
        {.name = "trailing blank lines", .parser = &json, .bytes = "{\"a\":1}\n\n\n", .lines = 3, .rows = 1},
        {.name = "a bad line", .parser = &json, .bytes = "{\"a\":1}\nnot json\n{\"a\":3}\n", .lines = 3, .rows = 2},
        {.name = "empty input", .parser = &json, .bytes = "", .lines = 0, .rows = 0},
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
    const std::string bytes = "{\"a\":1}\n" + std::string(200, 'x') + "\n{\"a\":3}\n";
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
    auto source = MemorySource(TIME_LINES, "batch_contract_time.jsonl");
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
        auto source = MemorySource(bytes, "batch_contract_time_keys.jsonl");
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
        std::string operator()(std::monostate /*unused*/) const
        {
            return "null";
        }
    };
    return std::visit(Visitor{}, value);
}

/** One text per row: every key of the parse in key text order, with the value and its type. */
[[nodiscard]] std::vector<std::string> DescribeRows(RecordingSink &sink)
{
    const KeyIndex &keys = sink.Keys();
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
    return Observed{
        .lastLine = sink.LastLineNumber(),
        .errors = std::move(errors),
        .rows = DescribeRows(sink),
        .newKeys = NewKeysOf(sink),
    };
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
        bytes += R"({"id":)" + std::to_string(i) + R"(,"msg":"line )" + std::to_string(i) + "\"";
        if (i % 7 == 0)
        {
            bytes += ",\"extra" + std::to_string(i) + "\":true";
        }
        if (i % 11 == 0)
        {
            bytes += R"(,"esc\u0041":1.5)";
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

TEST_CASE("A JSON number's type follows the value's own text", "[batch_contract][json_numbers]")
{
    const std::string bytes =
        "{\"n\":5.5}\n{\"n\":5}\n{\"n\":7.0}\n{\"n\":-3}\n{\"n\":18446744073709551615}\n{\"n\":5}\n{\"n\":6}\n"
        "{\"n\":1e2}\n{\"n\":5}\n{\"n\":18446744073709551615}\n{\"n\":1000000000000000000}\n";
    const std::vector<std::string> expected = {
        "n=d:5.500000;",
        "n=i:5;",
        "n=d:7.000000;",
        "n=i:-3;",
        "n=u:18446744073709551615;",
        "n=i:5;",
        "n=i:6;",
        "n=d:100.000000;",
        "n=i:5;",
        "n=u:18446744073709551615;",
        "n=i:1000000000000000000;",
    };

    SECTION("a file, one thread and several")
    {
        for (const unsigned int threads : {1U, 4U})
        {
            for (const std::size_t batchSize : {std::size_t{8}, std::size_t{0}})
            {
                RecordingSink sink;
                ParseAsFile(JsonParser{}, "batch_contract_numbers.jsonl", bytes, batchSize, sink, threads);
                INFO("threads " << threads << ", batch size " << batchSize);
                CHECK(DescribeRows(sink) == expected);
            }
        }
    }
    SECTION("a stream")
    {
        RecordingSink sink;
        ParseAsStream(JsonParser{}, bytes, 5, sink);
        CHECK(DescribeRows(sink) == expected);
    }
}

namespace
{

/** Collects what an export writes. */
class StringExportSink final : public exports::ExportSink
{
public:
    void Write(std::string_view bytes) override
    {
        text.append(bytes);
    }
    void Finish() override
    {
    }

    std::string text;
};

/**
 * Sixteen keys, each first seen in its own block of rows, so the order of the columns depends on which worker meets a
 * key first. The numeric key mixes 5 and 5.5 across and inside blocks, so its cell types depend on the cache of the
 * worker that parses a row. A block ends with a block of rows that hold every key.
 */
[[nodiscard]] std::string DeterminismInput()
{
    constexpr int KEY_COUNT = 16;
    constexpr int ROWS_PER_BLOCK = 400;
    std::string bytes;
    for (int block = 0; block <= KEY_COUNT; ++block)
    {
        const int keysInBlock = std::min(block + 1, KEY_COUNT);
        for (int row = 0; row < ROWS_PER_BLOCK; ++row)
        {
            bytes += "{\"id\":" + std::to_string((block * ROWS_PER_BLOCK) + row);
            bytes += (row % 3 == block % 2) ? ",\"num\":5" : ",\"num\":5.5";
            // Several keys start in the same block, in an order that differs from the order of their key text.
            for (int k = 0; k < keysInBlock; ++k)
            {
                const int key = (KEY_COUNT - 1) - k;
                bytes +=
                    ",\"key_" + std::to_string(key) + "\":\"value of the key in block " + std::to_string(block) + "\"";
            }
            bytes += "}\n";
        }
    }
    return bytes;
}

/** What a parse leaves in a table: the column order, the type of every cell, and the JSON Lines export. */
struct TableFacts
{
    std::vector<std::string> headers;
    std::vector<std::vector<std::size_t>> cellTypes;
    std::string jsonLines;
};

[[nodiscard]] TableFacts ParseIntoTable(const std::string &bytes, unsigned int threads, std::size_t batchSizeBytes)
{
    auto source = MemorySource(bytes, "batch_contract_determinism_" + std::to_string(threads) + ".jsonl");
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    ParserOptions options;
    options.threads = threads;
    options.batchSizeBytes = batchSizeBytes;
    AppendingSink sink(table);
    JsonParser{}.ParseStreaming(*sourcePtr, sink, options);

    TableFacts facts;
    const std::size_t columns = table.ColumnCount();
    for (std::size_t column = 0; column < columns; ++column)
    {
        facts.headers.push_back(table.GetHeader(column));
        std::vector<std::size_t> types;
        types.reserve(table.RowCount());
        for (std::size_t row = 0; row < table.RowCount(); ++row)
        {
            types.push_back(table.GetValue(row, column).index());
        }
        facts.cellTypes.push_back(std::move(types));
    }

    std::vector<std::size_t> rows(table.RowCount());
    std::iota(rows.begin(), rows.end(), std::size_t{0});
    std::vector<std::size_t> allColumns(columns);
    std::iota(allColumns.begin(), allColumns.end(), std::size_t{0});
    const exports::RowSource rowSource{
        .table = &table,
        .sourceRows = rows,
        .visibleColumns = allColumns,
        .includeAllFieldsForJson = true,
        .includeHeaderRow = false,
    };
    StringExportSink exportSink;
    exports::MakeExporter(exports::ExportFormat::JsonLines)->Run(rowSource, exportSink, StopToken{});
    facts.jsonLines = std::move(exportSink.text);
    return facts;
}

} // namespace

TEST_CASE(
    "The same file gives the same columns, cell types and export for every thread count and batch size",
    "[batch_contract][determinism]"
)
{
    const std::string bytes = DeterminismInput();
    const TableFacts expected = ParseIntoTable(bytes, 1, 1024 * 1024);
    REQUIRE(expected.headers.size() == 18);
    REQUIRE_FALSE(expected.jsonLines.empty());

    // The first row of the first block decides the order of the keys it holds, and a later key follows in input order.
    CHECK(expected.headers[0] == "id");
    CHECK(expected.headers[1] == "num");
    CHECK(expected.headers[2] == "key_15");

    for (const unsigned int threads : {1U, 2U, 8U})
    {
        for (const std::size_t batchSize : {std::size_t{64} * 1024, std::size_t{1024} * 1024})
        {
            for (int run = 0; run < 5; ++run)
            {
                const TableFacts actual = ParseIntoTable(bytes, threads, batchSize);
                INFO("threads " << threads << ", batch size " << batchSize << ", run " << run);
                CHECK(actual.headers == expected.headers);
                CHECK(actual.cellTypes == expected.cellTypes);
                CHECK(actual.jsonLines == expected.jsonLines);
            }
        }
    }
}

namespace
{

/** Counts the calls a driver makes, and can throw from OnBatch or OnFinished. */
class CountingSink final : public LogParseSink
{
public:
    enum class ThrowFrom
    {
        Nowhere,
        OnBatch,
        OnFinished,
    };

    explicit CountingSink(ThrowFrom throwFrom = ThrowFrom::Nowhere, int throwOnBatchCall = 2)
        : mThrowFrom(throwFrom), mThrowOnBatchCall(throwOnBatchCall)
    {
    }

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
        ++batchCalls;
        if (finished > 0 || finishedCalls > 0)
        {
            callAfterFinish = true;
        }
        if (mThrowFrom == ThrowFrom::OnBatch && batchCalls == mThrowOnBatchCall)
        {
            throw exports::BrokenPipe();
        }
        batches.push_back(std::move(batch));
    }

    void OnFinished(bool cancelled) override
    {
        ++finishedCalls;
        wasCancelled = cancelled;
        if (mThrowFrom == ThrowFrom::OnFinished)
        {
            throw exports::BrokenPipe();
        }
        ++finished;
    }

    std::vector<RowBatch> batches;
    int started = 0;
    int batchCalls = 0;
    int finishedCalls = 0;
    int finished = 0;
    bool wasCancelled = false;
    bool callAfterFinish = false;

    [[nodiscard]] const RowBatch &LastBatch() const
    {
        REQUIRE_FALSE(batches.empty());
        return batches.back();
    }

private:
    ThrowFrom mThrowFrom;
    int mThrowOnBatchCall;
    KeyIndex mKeys;
};

struct NoWorkerState
{
};

/** A Stage B that throws on one batch and otherwise only counts the lines of its slice. */
struct ThrowingStageB
{
    template <class Exception> struct Of
    {
        uint64_t throwOnBatch;
        Exception exception;

        void operator()(
            internal::ByteRange token,
            internal::WorkerScratch<NoWorkerState> & /*worker*/,
            KeyIndex & /*keys*/,
            std::span<const internal::TimeColumnSpec> /*timeColumns*/,
            internal::ParsedPipelineBatch &parsed
        ) const
        {
            if (token.batchIndex == throwOnBatch)
            {
                // The tests throw types that are not derived from std::exception on purpose.
                // NOLINTNEXTLINE(bugprone-std-exception-baseclass,bugprone-exception-copy-constructor-throws,cert-err60-cpp)
                throw exception;
            }
            parsed.batchIndex = token.batchIndex;
            parsed.totalLineCount = static_cast<std::size_t>(std::count(token.bytesBegin, token.bytesEnd, '\n'));
        }
    };
};

/** Runs the static driver over about 300 short lines with a Stage B that throws. */
template <class Exception> void RunStaticWithThrowingStageB(CountingSink &sink, Exception exception)
{
    std::string bytes;
    for (int i = 0; i < 300; ++i)
    {
        bytes += "line " + std::to_string(i) + "\n";
    }
    auto source = MemorySource(bytes, "batch_contract_throwing_static.log");
    const FileLineSource &logFile = *source;
    ParserOptions options;
    options.threads = 2;
    ThrowingStageB::Of<Exception> stageB{.throwOnBatch = 2, .exception = std::move(exception)};
    internal::RunStaticParserPipeline<internal::ByteRange, NoWorkerState>(
        *source,
        sink,
        options,
        internal::MakeLineAlignedBatcher(
            logFile.StableBytes().data(), logFile.StableBytes().data() + logFile.StableBytes().size(), 64
        ),
        stageB
    );
}

/** An exception type that is not derived from std::exception. */
struct NonStdException
{
    int code = 0;
};

/** A streaming decoder that throws on the line "boom" and otherwise emits an empty row. */
template <class Exception> class ThrowingLineDecoder
{
public:
    explicit ThrowingLineDecoder(Exception exception)
        : mException(std::move(exception))
    {
    }

    internal::LineDecodeResult DecodeCompact(
        std::string_view line,
        KeyIndex & /*keys*/,
        internal::PerWorkerKeyCache & /*keyCache*/,
        std::vector<std::pair<KeyId, CompactLogValue>> &out,
        std::string &outOwnedArena,
        std::string & /*errorOut*/
    )
    {
        out.clear();
        outOwnedArena.clear();
        if (line == "boom")
        {
            // NOLINTNEXTLINE(bugprone-std-exception-baseclass,bugprone-exception-copy-constructor-throws,cert-err60-cpp)
            throw mException;
        }
        return internal::LineDecodeResult::Emit;
    }

private:
    Exception mException;
};

template <class Exception> void RunStreamWithThrowingDecoder(CountingSink &sink, Exception exception)
{
    StreamLineSource source(
        "memory.log", std::make_unique<InMemoryBytesProducer>("one\ntwo\nthree\nboom\nfive\nsix\n", 4)
    );
    ThrowingLineDecoder<Exception> decoder(std::move(exception));
    internal::RunStreamingParseLoop(source, decoder, sink, ParserOptions{});
}

void CheckFinishedWithFailureError(const CountingSink &sink, const std::string &what)
{
    CHECK(sink.started == 1);
    CHECK(sink.finished == 1);
    CHECK(sink.finishedCalls == 1);
    CHECK_FALSE(sink.wasCancelled);
    CHECK_FALSE(sink.callAfterFinish);
    const auto &errors = sink.LastBatch().errors;
    REQUIRE(errors.size() >= 1);
    CHECK(errors.back().firstLine == 0);
    CHECK(errors.back().lastLine == 0);
    CHECK(errors.back().message == "Streaming parse failed: " + what);
}

} // namespace

TEST_CASE("A decoder that throws still finishes the sink, once, and the exception goes on", "[batch_contract][failure]")
{
    SECTION("the static driver")
    {
        CountingSink sink;
        CHECK_THROWS_WITH(RunStaticWithThrowingStageB(sink, std::runtime_error("decoder failed")), "decoder failed");
        CheckFinishedWithFailureError(sink, "decoder failed");
    }
    SECTION("the streaming driver")
    {
        CountingSink sink;
        CHECK_THROWS_WITH(RunStreamWithThrowingDecoder(sink, std::runtime_error("decoder failed")), "decoder failed");
        CheckFinishedWithFailureError(sink, "decoder failed");
        // The rows committed before the failing line were delivered, and the lines read so far are counted.
        std::size_t rows = 0;
        for (const RowBatch &batch : sink.batches)
        {
            rows += batch.lines.size();
        }
        CHECK(rows == 2);
        CHECK(sink.LastBatch().lastLineNumber == 4);
    }
    SECTION("an exception that is not a std::exception")
    {
        CountingSink sink;
        CHECK_THROWS(RunStreamWithThrowingDecoder(sink, NonStdException{42}));
        CheckFinishedWithFailureError(sink, "unknown exception");
    }
}

TEST_CASE("A cancelled operation finishes the sink as cancelled and goes on", "[batch_contract][failure]")
{
    SECTION("the static driver")
    {
        CountingSink sink;
        CHECK_THROWS_AS(RunStaticWithThrowingStageB(sink, OperationCancelled("stop")), OperationCancelled);
        CHECK(sink.finished == 1);
        CHECK(sink.wasCancelled);
        CHECK_FALSE(sink.callAfterFinish);
    }
    SECTION("the streaming driver")
    {
        CountingSink sink;
        CHECK_THROWS_AS(RunStreamWithThrowingDecoder(sink, OperationCancelled("stop")), OperationCancelled);
        CHECK(sink.finished == 1);
        CHECK(sink.wasCancelled);
        CHECK_FALSE(sink.callAfterFinish);
    }
}

TEST_CASE("A sink that threw is not called again", "[batch_contract][failure]")
{
    SECTION("from OnBatch, in the streaming driver")
    {
        CountingSink sink(CountingSink::ThrowFrom::OnBatch);
        std::string bytes;
        for (int i = 0; i < 2000; ++i)
        {
            bytes += "line " + std::to_string(i) + "\n";
        }
        StreamLineSource source("memory.log", std::make_unique<InMemoryBytesProducer>(bytes, 4096));
        ThrowingLineDecoder<std::runtime_error> decoder(std::runtime_error("unused"));
        // 250 rows make a batch, so the second batch throws on the sink's second call.
        CHECK_THROWS_AS(internal::RunStreamingParseLoop(source, decoder, sink, ParserOptions{}), exports::BrokenPipe);
        CHECK(sink.batchCalls == 2);
        CHECK(sink.finishedCalls == 0);
    }
    SECTION("from OnBatch, in the static driver")
    {
        // Stage B finds no rows here, so the only batch is the last one, sent when the pipeline finishes.
        CountingSink sink(CountingSink::ThrowFrom::OnBatch, 1);
        std::string bytes;
        for (int i = 0; i < 2000; ++i)
        {
            bytes += "line " + std::to_string(i) + "\n";
        }
        auto source = MemorySource(bytes, "batch_contract_sink_throws.log");
        const FileLineSource &logFile = *source;
        ThrowingStageB::Of<std::runtime_error> stageB{
            .throwOnBatch = UINT64_MAX,
            .exception = std::runtime_error("unused"),
        };
        CHECK_THROWS_AS(
            (internal::RunStaticParserPipeline<internal::ByteRange, NoWorkerState>(
                *source,
                sink,
                ParserOptions{},
                internal::MakeLineAlignedBatcher(
                    logFile.StableBytes().data(), logFile.StableBytes().data() + logFile.StableBytes().size(), 64
                ),
                stageB
            )),
            exports::BrokenPipe
        );
        CHECK(sink.batchCalls == 1);
        CHECK(sink.finishedCalls == 0);
    }
    SECTION("from OnFinished")
    {
        CountingSink sink(CountingSink::ThrowFrom::OnFinished);
        StreamLineSource source("memory.log", std::make_unique<InMemoryBytesProducer>("one\ntwo\n", 4));
        ThrowingLineDecoder<std::runtime_error> decoder(std::runtime_error("unused"));
        CHECK_THROWS_AS(internal::RunStreamingParseLoop(source, decoder, sink, ParserOptions{}), exports::BrokenPipe);
        CHECK(sink.finishedCalls == 1);
        CHECK(sink.batchCalls >= 1);
    }
}
