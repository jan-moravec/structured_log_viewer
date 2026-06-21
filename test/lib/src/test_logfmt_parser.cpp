#include "common.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/internal/buffering_sink.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/logfmt_parser.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>
#include <test_common/log_record.hpp>

#include <catch2/catch_all.hpp>
#include <glaze/glaze.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// logfmt fixtures are raw bytes on disk; `TestLogFile` (from common.hpp)
// writes them verbatim. Construct one, then `Write(...)` the record text.

TEST_CASE("Validate non-existent file [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    CHECK_FALSE(parser.IsValid("non_existent_file.logfmt"));
}

TEST_CASE("Validate empty file [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    CHECK_FALSE(parser.IsValid(file.GetFilePath()));
}

TEST_CASE("Validate JSON-shaped first line [logfmt]", "[logfmt_parser]")
{
    // A JSON-shaped first line must be rejected so JSON wins the
    // auto-detect race.
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    file.Write(R"({"key": "value"})"
               "\n");
    CHECK_FALSE(parser.IsValid(file.GetFilePath()));
}

TEST_CASE("Validate logfmt-shaped first line [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    file.Write("level=info msg=\"hello world\"\n");
    CHECK(parser.IsValid(file.GetFilePath()));
}

TEST_CASE("Validate file with no key=value pair [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    file.Write("just some plain text without equals\n");
    CHECK_FALSE(parser.IsValid(file.GetFilePath()));
}

TEST_CASE("Parse single bare key/value pair [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    file.Write("key=value\n");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("key")) == std::string_view{"value"});
}

TEST_CASE("Parse typed bare values [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    file.Write("i=-12 u=10000000000000000000 d=3.14 b=true f=false n=\n");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto values = result.data.Lines()[0].Values();
    CHECK(std::get<int64_t>(values.at("i")) == -12);
    CHECK(std::get<uint64_t>(values.at("u")) == 10000000000000000000ULL);
    CHECK(std::get<double>(values.at("d")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(values.at("b")) == true);
    CHECK(std::get<bool>(values.at("f")) == false);
    CHECK(std::holds_alternative<std::monostate>(values.at("n")));
}

TEST_CASE("Quoted values stay strings [logfmt]", "[logfmt_parser]")
{
    // `pid="42"` must stay the string "42", not promote to int.
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    file.Write("pid=\"42\" msg=\"hello world\"\n");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto values = result.data.Lines()[0].Values();
    CHECK(loglib::AsStringView(values.at("pid")) == std::string_view{"42"});
    CHECK(loglib::AsStringView(values.at("msg")) == std::string_view{"hello world"});
}

TEST_CASE("Quoted value with C-style escapes [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    file.Write("msg=\"a \\\"quoted\\\" word\\nnew line\\ttab\\\\back\"\n");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto values = result.data.Lines()[0].Values();
    const std::string expected = "a \"quoted\" word\nnew line\ttab\\back";
    CHECK(loglib::AsStringView(values.at("msg")) == std::string_view{expected});
}

TEST_CASE("Unterminated quoted value reports a parse error [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    file.Write("key=\"unterminated\n");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    CHECK(result.errors.size() == 1);
    CHECK(result.errors[0].contains("Unterminated quoted value"));
}

TEST_CASE("Bare key with no '=' is treated as null [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    file.Write("flag level=info\n");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto values = result.data.Lines()[0].Values();
    CHECK(std::holds_alternative<std::monostate>(values.at("flag")));
    CHECK(loglib::AsStringView(values.at("level")) == std::string_view{"info"});
}

TEST_CASE("Repeated keys: last write wins [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    file.Write("key=first key=second key=third\n");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("key")) == std::string_view{"third"});
}

TEST_CASE("Multiple lines parse independently [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    file.Write("level=info msg=\"first line\"\n"
               "level=warn msg=\"second line\" code=42\n"
               "level=error msg=\"third line\"\n");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 3);

    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("level")) == std::string_view{"info"});
    CHECK(loglib::AsStringView(result.data.Lines()[1].GetValue("msg")) == std::string_view{"second line"});
    CHECK(std::get<uint64_t>(result.data.Lines()[1].GetValue("code")) == 42u);
    CHECK(loglib::AsStringView(result.data.Lines()[2].GetValue("level")) == std::string_view{"error"});
}

TEST_CASE("Blank lines are skipped [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    file.Write("\n\nkey=value\n\n");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("key")) == std::string_view{"value"});
}

TEST_CASE("Last line lacks trailing newline [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    file.Write("a=1\nb=2");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(std::get<uint64_t>(result.data.Lines()[0].GetValue("a")) == 1u);
    CHECK(std::get<uint64_t>(result.data.Lines()[1].GetValue("b")) == 2u);
}

TEST_CASE("Plain text is parsed permissively as null-valued bare keys [logfmt]", "[logfmt_parser]")
{
    // kr/logfmt treats each whitespace-separated word as a
    // (key, null) pair, so plain prose is "valid" logfmt with N
    // null-valued bare keys. We mirror that.
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    file.Write("level=info\nplain text line\nlevel=error\n");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 3);
    CHECK(std::holds_alternative<std::monostate>(result.data.Lines()[1].GetValue("plain")));
    CHECK(std::holds_alternative<std::monostate>(result.data.Lines()[1].GetValue("text")));
}

TEST_CASE("Line of only '=' / '\"' surfaces as parse error [logfmt]", "[logfmt_parser]")
{
    // No emitable key/value pairs: the scanner should report the
    // line instead of silently producing an empty record.
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    file.Write("level=info\n====\nlevel=error\n");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors[0].contains("line 2"));
    CHECK(result.errors[0].contains("Not a logfmt record"));
    REQUIRE(result.data.Lines().size() == 2);
}

TEST_CASE("ToString round-trips bare and quoted values [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;
    const TestLogFile file;
    file.Write("level=info msg=\"hello world\" code=42 ratio=3.14\n");

    auto result = loglib::ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const std::string out = parser.ToString(result.data.Lines()[0]);
    // IndexedValues are KeyId-ordered (insertion order via `KeyIndex`).
    CHECK(out.contains("level=info"));
    CHECK(out.contains("msg=\"hello world\""));
    CHECK(out.contains("code=42"));
    CHECK(out.contains("ratio=3.14"));
}

TEST_CASE("ToString quotes values with whitespace and special bytes [logfmt]", "[logfmt_parser]")
{
    using namespace loglib;
    const std::string emitted = LogfmtParser::ToString(LogMap{
        {"safe", LogValue{std::string_view{"value"}}},
        {"spaced", LogValue{std::string_view{"hello world"}}},
        {"with_quote", LogValue{std::string_view{"a\"b"}}},
        {"with_newline", LogValue{std::string_view{"line1\nline2"}}}
    });

    CHECK(emitted.contains("safe=value"));
    CHECK(emitted.contains("spaced=\"hello world\""));
    CHECK(emitted.contains("with_quote=\"a\\\"b\""));
    CHECK(emitted.contains("with_newline=\"line1\\nline2\""));
}

TEST_CASE("Parse file via FactoryParser auto-detect [logfmt]", "[logfmt_parser]")
{
    const TestLogFile file;
    file.Write("level=info msg=\"hello\"\n");
    auto result = loglib::ParseFile(file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(loglib::AsStringView(result.data.Lines()[0].GetValue("msg")) == std::string_view{"hello"});
}

// The `test_common::Logfmt()` writer in `test/common/src/log_format.cpp`
// duplicates `loglib::BareValueIsSafe` and `loglib::AppendQuotedString`
// (deliberately, so `test_common` stays loglib-free). Drift between the two
// would silently invalidate every `[logfmt_parser]` benchmark fixture, since
// nothing else exercises both sides. The two cases below pin that contract
// by writing a record through the test serializer and asserting every value
// family parses back through `loglib::LogfmtParser`.
TEST_CASE(
    "test_common::Logfmt() writes round-trip through LogfmtParser (all value families) [logfmt]",
    "[logfmt_parser][round_trip]"
)
{
    using namespace loglib;

    // Each field exercises one branch of `AppendLogfmtValue` / a parser
    // typed-bare-value path:
    //   bare     -> bare-safe string (no quoting on either side)
    //   spaced   -> string with whitespace -> AppendQuotedString
    //   dquote   -> string with '"'        -> escape via \"
    //   bslash   -> string with '\\'       -> escape via \\
    //   newline  -> string with '\n'       -> escape via \n
    //   creturn  -> string with '\r'       -> escape via \r
    //   tab      -> string with '\t'       -> escape via \t
    //   ineg     -> negative int64_t       -> bare negative -> int64_t
    //   upos     -> uint64_t > INT64_MAX   -> bare unsigned -> uint64_t
    //   dbl      -> double                 -> bare numeric  -> double
    //   btrue    -> true                   -> bare true     -> bool
    //   bfalse   -> false                  -> bare false    -> bool
    //   nullv    -> null                   -> empty value   -> monostate
    test_common::LogRecord record;
    record["bare"] = std::string("info");
    record["spaced"] = std::string("hello world");
    record["dquote"] = std::string("a\"b");
    record["bslash"] = std::string("a\\b");
    record["newline"] = std::string("line1\nline2");
    record["creturn"] = std::string("line1\rline2");
    record["tab"] = std::string("col1\tcol2");
    record["ineg"] = static_cast<std::int64_t>(-42);
    record["upos"] = static_cast<std::uint64_t>(10000000000000000000ULL);
    record["dbl"] = 3.14;
    record["btrue"] = true;
    record["bfalse"] = false;
    record["nullv"] = nullptr;

    const test_common::LogFormat format = test_common::Logfmt();
    const std::string serialized = format.writeLine(record);

    const TestLogFile file;
    file.Write(serialized + "\n");

    const LogfmtParser parser;
    const ParseResult result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto values = result.data.Lines()[0].Values();
    CHECK(AsStringView(values.at("bare")) == std::string_view{"info"});
    CHECK(AsStringView(values.at("spaced")) == std::string_view{"hello world"});
    CHECK(AsStringView(values.at("dquote")) == std::string_view{"a\"b"});
    CHECK(AsStringView(values.at("bslash")) == std::string_view{"a\\b"});
    CHECK(AsStringView(values.at("newline")) == std::string_view{"line1\nline2"});
    CHECK(AsStringView(values.at("creturn")) == std::string_view{"line1\rline2"});
    CHECK(AsStringView(values.at("tab")) == std::string_view{"col1\tcol2"});
    CHECK(std::get<std::int64_t>(values.at("ineg")) == -42);
    CHECK(std::get<std::uint64_t>(values.at("upos")) == 10000000000000000000ULL);
    CHECK(std::get<double>(values.at("dbl")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(values.at("btrue")) == true);
    CHECK(std::get<bool>(values.at("bfalse")) == false);
    CHECK(std::holds_alternative<std::monostate>(values.at("nullv")));
}

TEST_CASE(
    "test_common::Logfmt() round-trips the generator's record shape end-to-end [logfmt]",
    "[logfmt_parser][round_trip]"
)
{
    // Drive the actual benchmark fixture through write+parse so a future
    // tweak to `GenerateRandomLogRecord` (e.g. adding a field that contains
    // an unescapable byte) lands here rather than as a silent benchmark
    // skew.
    using namespace loglib;

    constexpr std::size_t LINE_COUNT = 32;
    constexpr std::uint32_t SEED = 0xBA0BABu;

    // Constant seed is intentional: the test asserts byte-equal round-trips
    // against the exact records the generator produces.
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    std::mt19937 rng(SEED);
    std::vector<test_common::LogRecord> records;
    records.reserve(LINE_COUNT);
    for (std::size_t i = 0; i < LINE_COUNT; ++i)
    {
        records.emplace_back(test_common::GenerateRandomLogRecord(rng, i));
    }

    // Move the records into the fixture and read them back through
    // `Records()` so we don't keep two copies of the generated vector alive.
    const TestStructuredLogFile fixture(std::move(records), test_common::Logfmt());

    const LogfmtParser parser;
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

        // Every key the generator emits must round-trip. String fields
        // (timestamp/level/message/component) compare via `AsStringView`;
        // `thread_id` is non-negative so the parser bare-value rule maps
        // it to `uint64_t`.
        CHECK(AsStringView(values.at("timestamp")) == std::string_view{record["timestamp"].get_string()});
        CHECK(AsStringView(values.at("level")) == std::string_view{record["level"].get_string()});
        CHECK(AsStringView(values.at("message")) == std::string_view{record["message"].get_string()});
        CHECK(AsStringView(values.at("component")) == std::string_view{record["component"].get_string()});

        const auto expectedThreadId = static_cast<std::uint64_t>(i % 16);
        CHECK(std::get<std::uint64_t>(values.at("thread_id")) == expectedThreadId);
    }
}
