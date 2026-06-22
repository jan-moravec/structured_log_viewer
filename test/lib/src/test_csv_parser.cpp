#include "common.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/line_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/log_table.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/csv_parser.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>
#include <test_common/log_record.hpp>

#include <catch2/catch_all.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

TEST_CASE("Validate non-existent file [csv]", "[csv_parser]")
{
    const loglib::CsvParser parser;
    CHECK_FALSE(parser.IsValid("non_existent_file.csv"));
}

TEST_CASE("Validate empty file [csv]", "[csv_parser]")
{
    const loglib::CsvParser parser;
    const TestLogFile file;
    CHECK_FALSE(parser.IsValid(file.GetFilePath()));
}

TEST_CASE("Validate header-only file [csv]", "[csv_parser]")
{
    // Header but no data row -> reject. The false-positive guard
    // requires a second non-blank line to confirm the cell count.
    const loglib::CsvParser parser;
    const TestLogFile file;
    file.Write("a,b,c\n");
    CHECK_FALSE(parser.IsValid(file.GetFilePath()));
}

TEST_CASE("Validate JSON-shaped first line [csv]", "[csv_parser]")
{
    // The cell count for a JSON line is typically 1 (no comma), so
    // `HeaderLineLooksLikeCsv` rejects it. This is what makes JSON
    // win the auto-detect race for `{"key": "value"}`-shaped input.
    const loglib::CsvParser parser;
    const TestLogFile file;
    file.Write(R"({"key": "value"})"
               "\n"
               R"({"key": "value2"})"
               "\n");
    CHECK_FALSE(parser.IsValid(file.GetFilePath()));
}

TEST_CASE("Validate logfmt-shaped first line [csv]", "[csv_parser]")
{
    // logfmt lines have no commas in their tokenised cell count, so
    // CSV's header probe rejects them and logfmt wins the auto-detect.
    const loglib::CsvParser parser;
    const TestLogFile file;
    file.Write("level=info msg=hello\nlevel=warn msg=world\n");
    CHECK_FALSE(parser.IsValid(file.GetFilePath()));
}

TEST_CASE("Validate well-formed CSV [csv]", "[csv_parser]")
{
    const loglib::CsvParser parser;
    const TestLogFile file;
    file.Write("level,message\ninfo,hello\n");
    CHECK(parser.IsValid(file.GetFilePath()));
}

TEST_CASE("Validate file with mismatched cell count [csv]", "[csv_parser]")
{
    const loglib::CsvParser parser;
    const TestLogFile file;
    file.Write("level,message,extra\ninfo,hello\n");
    CHECK_FALSE(parser.IsValid(file.GetFilePath()));
}

namespace
{
// Minimal `LogParseSink` that forwards batches straight into a
// `LogTable`, mirroring the GUI's `QtStreamingLogSink` -> `LogModel`
// path. Used to exercise column creation (which only happens via
// `StreamedBatch::newKeys`), unlike the value-only `BufferingSink`
// that `ParseFile` uses.
struct TableSink : loglib::LogParseSink
{
    loglib::LogTable *table = nullptr;
    loglib::KeyIndex &Keys() override
    {
        return table->Keys();
    }
    void OnStarted() override
    {
    }
    void OnBatch(loglib::StreamedBatch batch) override
    {
        table->AppendBatch(std::move(batch));
    }
    void OnFinished(bool /*cancelled*/) override
    {
    }
};
} // namespace

TEST_CASE("Static parse builds LogTable columns from the CSV header [csv]", "[csv_parser]")
{
    // Regression guard: the static (`File -> Open...`) pipeline interns
    // the header columns *before* the pipeline's `BatchCoalescer`
    // snapshots its new-key baseline, so without the explicit baseline
    // hand-off the coalescer would emit zero `newKeys` and the
    // `LogTable` would show rows with no columns at all.
    //
    // NOTE: We construct the `FileLineSource` directly from
    // `std::make_unique<LogFile>(path)` rather than going through
    // `TestLogFile::CreateFileLineSource()`. That helper eagerly
    // pre-scans the file and stuffs the resulting offsets into the
    // `LogFile`; the static pipeline *also* writes its own offsets
    // back into the same `LogFile`, which collides with the
    // strictly-increasing assertion in `LogFile::AppendLineOffsets`
    // (debug builds).
    const TestLogFile file("static_columns.csv");
    file.Write("level,message\ninfo,hello\nerror,boom\n");

    loglib::LogTable table{loglib::LogData{}, loglib::LogConfigurationManager{}};
    auto source = std::make_unique<loglib::FileLineSource>(std::make_unique<loglib::LogFile>(file.GetFilePath()));
    loglib::FileLineSource *sourceRaw = source.get();
    table.BeginStreaming(std::move(source));

    TableSink sink;
    sink.table = &table;
    loglib::CsvParser::ParseStreaming(
        *sourceRaw, sink, loglib::ParserOptions{}, loglib::internal::AdvancedParserOptions{}
    );

    CHECK(table.RowCount() == 2);
    CHECK(table.ColumnCount() == 2);
}

TEST_CASE("Parse minimal CSV [csv]", "[csv_parser]")
{
    const loglib::CsvParser parser;
    const TestLogFile file;
    file.Write("level,message\ninfo,hello\nerror,boom\n");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("level")) == std::string_view{"info"});
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("message")) == std::string_view{"hello"});
    CHECK(loglib::AsStringView(result.data.Lines()[1].GetValue("level")) == std::string_view{"error"});
    CHECK(loglib::AsStringView(result.data.Lines()[1].GetValue("message")) == std::string_view{"boom"});
}

TEST_CASE("Parse typed bare cells [csv]", "[csv_parser]")
{
    // Mirror logfmt's bare-value rules: bools, signed/unsigned ints,
    // doubles all type-classify; quoted cells stay strings.
    const loglib::CsvParser parser;
    const TestLogFile file;
    file.Write(
        "i,u,d,b,f,n,q\n"
        "-12,10000000000000000000,3.14,true,false,,\"42\"\n"
    );

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto &row = result.data.Lines()[0];
    CHECK(std::get<std::int64_t>(row.GetValue("i")) == -12);
    CHECK(std::get<std::uint64_t>(row.GetValue("u")) == 10000000000000000000ULL);
    CHECK(std::get<double>(row.GetValue("d")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(row.GetValue("b")) == true);
    CHECK(std::get<bool>(row.GetValue("f")) == false);
    // `n` is the empty cell -> omitted from the row. `GetValue` of an
    // absent key returns monostate; `Values()` doesn't contain it at all.
    CHECK(std::holds_alternative<std::monostate>(row.GetValue("n")));
    CHECK_FALSE(row.Values().contains("n"));
    // `q` was `"42"` -> stays string, doesn't promote to int.
    CHECK(loglib::AsStringView(row.GetValue("q")) == std::string_view{"42"});
}

TEST_CASE("Quoted cell with embedded comma / quote / newline [csv]", "[csv_parser]")
{
    // `""` escapes a literal `"` inside a quoted cell. Embedded
    // commas pass through unchanged because the quote brackets the
    // cell. Embedded newlines in a single-line quoted cell are NOT
    // supported (v1 limitation) -- here we only test `,` and `""`.
    const loglib::CsvParser parser;
    const TestLogFile file;
    file.Write(
        "label,text\n"
        "comma,\"a,b\"\n"
        "quote,\"a\"\"b\"\n"
    );

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("text")) == std::string_view{"a,b"});
    CHECK(loglib::AsStringView(result.data.Lines()[1].GetValue("text")) == std::string_view{"a\"b"});
}

TEST_CASE("Unterminated quoted cell surfaces as error [csv]", "[csv_parser]")
{
    const loglib::CsvParser parser;
    const TestLogFile file;
    file.Write(
        "a,b\n"
        "ok,ok\n"
        "broken,\"unterminated\n"
    );

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.size() == 1);
    // Line numbers in errors are 1-based and include the header --
    // so the first data row is "line 2", second data row "line 3".
    CHECK(result.errors[0].contains("line 3"));
    CHECK(result.errors[0].contains("Unterminated quoted value"));
    CHECK(result.data.Lines().size() == 1);
}

TEST_CASE("Ragged row with extra cells surfaces as error [csv]", "[csv_parser]")
{
    const loglib::CsvParser parser;
    const TestLogFile file;
    file.Write(
        "a,b\n"
        "1,2\n"
        "1,2,3\n"
    );

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors[0].contains("line 3"));
    CHECK(result.errors[0].contains("extra cell"));
    CHECK(result.data.Lines().size() == 1);
}

TEST_CASE("Empty cells are omitted, missing trailing cells materialise as monostate [csv]", "[csv_parser]")
{
    // `a,,c` -> middle cell omitted. `a,b` (one cell short of the
    // header) -> trailing cell absent; lookup yields monostate.
    const loglib::CsvParser parser;
    const TestLogFile file;
    file.Write(
        "a,b,c\n"
        "1,,3\n"
        "4,5\n"
    );

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);

    const auto &row0 = result.data.Lines()[0];
    CHECK(std::get<std::uint64_t>(row0.GetValue("a")) == 1u);
    // Empty cell -> key omitted from row entirely.
    CHECK(std::holds_alternative<std::monostate>(row0.GetValue("b")));
    CHECK_FALSE(row0.Values().contains("b"));
    CHECK(std::get<std::uint64_t>(row0.GetValue("c")) == 3u);

    const auto &row1 = result.data.Lines()[1];
    CHECK(std::get<std::uint64_t>(row1.GetValue("a")) == 4u);
    CHECK(std::get<std::uint64_t>(row1.GetValue("b")) == 5u);
    // Missing trailing cell -> key absent from the row.
    CHECK(std::holds_alternative<std::monostate>(row1.GetValue("c")));
    CHECK_FALSE(row1.Values().contains("c"));
}

TEST_CASE("UTF-8 BOM on the header is stripped [csv]", "[csv_parser]")
{
    const loglib::CsvParser parser;
    const TestLogFile file;
    // Three bytes EF BB BF before `level`.
    file.Write("\xEF\xBB\xBFlevel,message\ninfo,hello\n");
    REQUIRE(parser.IsValid(file.GetFilePath()));

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("level")) == std::string_view{"info"});
}

TEST_CASE("CRLF line endings work [csv]", "[csv_parser]")
{
    const loglib::CsvParser parser;
    const TestLogFile file;
    file.Write("a,b\r\n1,2\r\n3,4\r\n");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(std::get<std::uint64_t>(result.data.Lines()[0].GetValue("b")) == 2u);
    CHECK(std::get<std::uint64_t>(result.data.Lines()[1].GetValue("b")) == 4u);
}

TEST_CASE("Last data line lacks trailing newline [csv]", "[csv_parser]")
{
    const loglib::CsvParser parser;
    const TestLogFile file;
    file.Write("a,b\n1,2\n3,4");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(std::get<std::uint64_t>(result.data.Lines()[1].GetValue("a")) == 3u);
    CHECK(std::get<std::uint64_t>(result.data.Lines()[1].GetValue("b")) == 4u);
}

TEST_CASE("LineSource::RawLine alignment skips the header [csv]", "[csv_parser]")
{
    // After the header is swallowed, each `LogLine`'s `(Source, LineId)`
    // pair should resolve back to the data row's on-disk bytes via
    // `LineSource::RawLine`. The header is line 0 (id 0); the first
    // data row is id 1, the second is id 2.
    const loglib::CsvParser parser;
    const TestLogFile file;
    file.Write("a,b,c\n10,20,30\n40,50,60\n");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);

    const auto &line0 = result.data.Lines()[0];
    REQUIRE(line0.Source() != nullptr);
    CHECK(line0.Source()->RawLine(line0.LineId()) == std::string{"10,20,30"});

    const auto &line1 = result.data.Lines()[1];
    REQUIRE(line1.Source() != nullptr);
    CHECK(line1.Source()->RawLine(line1.LineId()) == std::string{"40,50,60"});
}

TEST_CASE("ToString emits cells comma-separated, quoting where needed [csv]", "[csv_parser]")
{
    using namespace loglib;
    const std::string emitted = CsvParser::ToString(LogMap{
        {"a_safe", LogValue{std::string_view{"value"}}},
        {"b_with_comma", LogValue{std::string_view{"a,b"}}},
        {"c_with_quote", LogValue{std::string_view{"a\"b"}}},
        {"d_with_newline", LogValue{std::string_view{"line1\nline2"}}},
        {"e_int", LogValue{static_cast<std::int64_t>(-42)}},
        {"f_bool", LogValue{true}}
    });

    // CSV data rows have no inline keys; `ToString(LogMap)` emits the
    // **values only**, lex-key-ordered. With the keys above (already
    // in lex order via the `a_` / `b_` / ... prefixes), the emitted
    // row is exactly the cell sequence below.
    CHECK(emitted == "value,\"a,b\",\"a\"\"b\",\"line1\nline2\",-42,true");
}

TEST_CASE("Parse file via FactoryParser auto-detect [csv]", "[csv_parser]")
{
    const TestLogFile file;
    file.Write("level,message\ninfo,hello\n");
    auto result = loglib::ParseFile(file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("message")) == std::string_view{"hello"});
}

// The `test_common::Csv()` writer duplicates `loglib`'s
// `BareCellIsSafe` / `AppendQuotedCell`. The round-trip test below
// pins the two copies in sync; otherwise the upcoming benchmark
// fixtures would silently disagree with the parser.
TEST_CASE(
    "test_common::Csv() writes round-trip through CsvParser (all value families) [csv]",
    "[csv_parser][round_trip]"
)
{
    using namespace loglib;

    test_common::LogRecord record;
    record["bare"] = std::string("info");
    record["spaced"] = std::string("hello world");
    record["with_comma"] = std::string("a,b");
    record["with_quote"] = std::string("a\"b");
    record["ineg"] = static_cast<std::int64_t>(-42);
    record["upos"] = static_cast<std::uint64_t>(10000000000000000000ULL);
    record["dbl"] = 3.14;
    record["btrue"] = true;
    record["bfalse"] = false;
    record["nullv"] = nullptr;

    // Schema in lex order (matches `DeriveSchemaFromRecord`).
    const auto schema = test_common::DeriveSchemaFromRecord(record);
    REQUIRE_FALSE(schema.empty());
    const test_common::LogFormat format = test_common::Csv(schema);

    const std::string headerLine = format.writeHeader(schema);
    const std::string dataLine = format.writeLine(record);

    const TestLogFile file;
    file.Write(headerLine + "\n" + dataLine + "\n");

    const CsvParser parser;
    REQUIRE(parser.IsValid(file.GetFilePath()));
    const ParseResult result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto &row = result.data.Lines()[0];
    CHECK(AsStringView(row.GetValue("bare")) == std::string_view{"info"});
    CHECK(AsStringView(row.GetValue("spaced")) == std::string_view{"hello world"});
    CHECK(AsStringView(row.GetValue("with_comma")) == std::string_view{"a,b"});
    CHECK(AsStringView(row.GetValue("with_quote")) == std::string_view{"a\"b"});
    CHECK(std::get<std::int64_t>(row.GetValue("ineg")) == -42);
    CHECK(std::get<std::uint64_t>(row.GetValue("upos")) == 10000000000000000000ULL);
    CHECK(std::get<double>(row.GetValue("dbl")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(row.GetValue("btrue")) == true);
    CHECK(std::get<bool>(row.GetValue("bfalse")) == false);
    // null serialised to an empty cell -> omitted from the row.
    // `GetValue` of an absent key returns monostate.
    CHECK(std::holds_alternative<std::monostate>(row.GetValue("nullv")));
    CHECK_FALSE(row.Values().contains("nullv"));
}

TEST_CASE("test_common::Csv() round-trips the generator's record shape end-to-end [csv]", "[csv_parser][round_trip]")
{
    // Mirrors the logfmt end-to-end round-trip but for CSV: drives the
    // benchmark generator through write+parse so a future tweak to
    // `GenerateRandomLogRecord` lands here rather than as silent skew
    // in `[csv_parser][large]` numbers.
    using namespace loglib;

    constexpr std::size_t LINE_COUNT = 32;
    constexpr std::uint32_t SEED = 0xBA0BABu;

    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    std::mt19937 rng(SEED);
    std::vector<test_common::LogRecord> records;
    records.reserve(LINE_COUNT);
    for (std::size_t i = 0; i < LINE_COUNT; ++i)
    {
        records.emplace_back(test_common::GenerateRandomLogRecord(rng, i));
    }
    REQUIRE(records.front().is_object());
    const auto schema = test_common::DeriveSchemaFromRecord(records.front());

    const TestStructuredLogFile fixture(std::move(records), test_common::Csv(schema), schema);

    const CsvParser parser;
    const ParseResult result = ParseFile(parser, fixture.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == LINE_COUNT);

    const auto &fixtureRecords = fixture.Records();
    REQUIRE(fixtureRecords.size() == LINE_COUNT);
    for (std::size_t i = 0; i < LINE_COUNT; ++i)
    {
        INFO("row " << i);
        const auto values = result.data.Lines()[i].Values();
        const auto &record = fixtureRecords[i];
        REQUIRE(record.is_object());

        CHECK(AsStringView(values.at("timestamp")) == std::string_view{record["timestamp"].get_string()});
        CHECK(AsStringView(values.at("level")) == std::string_view{record["level"].get_string()});
        CHECK(AsStringView(values.at("message")) == std::string_view{record["message"].get_string()});
        CHECK(AsStringView(values.at("component")) == std::string_view{record["component"].get_string()});

        const auto expectedThreadId = static_cast<std::uint64_t>(i % 16);
        CHECK(std::get<std::uint64_t>(values.at("thread_id")) == expectedThreadId);
    }
}

TEST_CASE("Static parse skips the header even when leading blanks push it past batch 0 [csv]", "[csv_parser]")
{
    // Regression guard for the "header treated as a data row" bug: the
    // old `headerToSkip = (batchIndex == 0)` heuristic only swallowed
    // the first non-blank line in batch 0. With a small `batchSizeBytes`
    // (or just enough leading blank lines), the header could land in
    // batch 1+ and surface as a spurious row whose cell values were the
    // column names. The fix records the header's byte offset eagerly
    // and Stage B skips that exact offset regardless of batching.
    //
    // Source is built directly from `LogFile` (see the note on
    // "Static parse builds LogTable columns from the CSV header" for
    // why this bypasses `TestLogFile::CreateFileLineSource()`).
    using namespace loglib;

    std::string content;
    for (int i = 0; i < 40; ++i)
    {
        content.push_back('\n');
    }
    content += "level,message\ninfo,hello\nerror,boom\n";

    const TestLogFile file("header_in_late_batch.csv");
    file.Write(content);

    LogTable table{LogData{}, LogConfigurationManager{}};
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(file.GetFilePath()));
    FileLineSource *sourceRaw = source.get();
    table.BeginStreaming(std::move(source));

    TableSink sink;
    sink.table = &table;

    internal::AdvancedParserOptions advanced;
    advanced.threads = 1;
    advanced.batchSizeBytes = 16; // forces header into a non-zero batch
    CsvParser::ParseStreaming(*sourceRaw, sink, ParserOptions{}, advanced);

    CHECK(table.RowCount() == 2);
    CHECK(table.ColumnCount() == 2);
}

TEST_CASE("Malformed CSV header surfaces a single error and zero data rows [csv]", "[csv_parser]")
{
    // Regression guard for the silent-malformed-header bug: the eager
    // header parse used to `(void)` its return value, so an
    // unterminated-quote header was swallowed and every data row
    // surfaced one `Row has N extra cell(s) beyond the 0-column header.`
    // error. The fix short-circuits with a single
    // `Invalid CSV header.` diagnostic.
    using namespace loglib;

    const TestLogFile file("malformed_header.csv");
    // Unterminated quote on the header (`"name` never closes).
    // `IsValid` rejects this, so we drive the parser directly via
    // `ParseFile(parser, path)` rather than the auto-detect overload.
    file.Write("\"name,value\ninfo,hello\nerror,boom\n");

    const CsvParser parser;
    CHECK_FALSE(parser.IsValid(file.GetFilePath()));

    const ParseResult result = ParseFile(parser, file.GetFilePath());
    CHECK(result.data.Lines().empty());
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors[0].contains("Invalid CSV header"));
    CHECK(result.errors[0].contains("line 1"));
}

TEST_CASE("Malformed CSV header after blank lines reports the right line number [csv]", "[csv_parser]")
{
    // The blank-line counter in the eager parse must surface in the
    // error message: two leading blank lines + a malformed header on
    // line 3 should say `Error on line 3: Invalid CSV header.`
    using namespace loglib;

    const TestLogFile file("malformed_header_after_blanks.csv");
    file.Write("\n\n\"name,value\ninfo,hello\n");

    const CsvParser parser;
    const ParseResult result = ParseFile(parser, file.GetFilePath());
    CHECK(result.data.Lines().empty());
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors[0].contains("Invalid CSV header"));
    CHECK(result.errors[0].contains("line 3"));
}

TEST_CASE("Duplicate CSV column names are silently renamed [csv]", "[csv_parser]")
{
    // Regression guard for the duplicate-header bug: without dedupe,
    // `id,name,id` would intern the same KeyId twice in `columnKeys`,
    // producing a `LogLine` with two entries for the same key. The
    // fix renames the second occurrence to `id_2` so both data slots
    // remain addressable.
    using namespace loglib;

    const TestLogFile file("duplicate_headers.csv");
    file.Write("id,name,id\n1,foo,2\n3,bar,4\n");

    const CsvParser parser;
    REQUIRE(parser.IsValid(file.GetFilePath()));

    const ParseResult result = ParseFile(parser, file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);

    const auto &row0 = result.data.Lines()[0];
    CHECK(std::get<std::uint64_t>(row0.GetValue("id")) == 1u);
    CHECK(AsStringView(row0.GetValue("name")) == std::string_view{"foo"});
    CHECK(std::get<std::uint64_t>(row0.GetValue("id_2")) == 2u);

    const auto &row1 = result.data.Lines()[1];
    CHECK(std::get<std::uint64_t>(row1.GetValue("id")) == 3u);
    CHECK(AsStringView(row1.GetValue("name")) == std::string_view{"bar"});
    CHECK(std::get<std::uint64_t>(row1.GetValue("id_2")) == 4u);
}

TEST_CASE("Duplicate CSV column names skip already-taken suffixes [csv]", "[csv_parser]")
{
    // `id, id_2, id` -- the third occurrence cannot take `id_2`
    // because column 1 already used it, so dedupe must skip to
    // `id_3`. Pins the suffix-probe loop in `DeduplicateColumnNames`.
    using namespace loglib;

    const TestLogFile file("duplicate_headers_skip.csv");
    file.Write("id,id_2,id\n1,2,3\n");

    const CsvParser parser;
    const ParseResult result = ParseFile(parser, file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto &row = result.data.Lines()[0];
    CHECK(std::get<std::uint64_t>(row.GetValue("id")) == 1u);
    CHECK(std::get<std::uint64_t>(row.GetValue("id_2")) == 2u);
    CHECK(std::get<std::uint64_t>(row.GetValue("id_3")) == 3u);
}
