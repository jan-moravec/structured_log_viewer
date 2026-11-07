#include "common.hpp"
#include "parse_only.hpp"

#include <loglib/core/bytes_producer.hpp>
#include <loglib/core/file_line_source.hpp>
#include <loglib/core/log_data.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/log_parser.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/parse/parsers/logfmt_parser.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>
#include <test_common/log_record.hpp>

#include <catch2/catch_all.hpp>
#include <glaze/glaze.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// logfmt fixtures are raw bytes; use `TestLogFile::Write(...)` from
// common.hpp. Record-driven fixtures go through `TestStructuredLogFile`.

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
    file.Write(
        R"({"key": "value"})"
        "\n"
    );
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

    auto result = loglib::ParseSource(parser, MemorySource("key=value\n"));
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(
        loglib::AsStringView(result.data.Lines()[0].GetValue(result.data.Keys(), "key")) == std::string_view{"value"}
    );
}

TEST_CASE("Parse typed bare values [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;

    auto result = loglib::ParseSource(parser, MemorySource("i=-12 u=10000000000000000000 d=3.14 b=true f=false n=\n"));
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto values = result.data.Lines()[0].Values(result.data.Keys());
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

    auto result = loglib::ParseSource(parser, MemorySource("pid=\"42\" msg=\"hello world\"\n"));
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto values = result.data.Lines()[0].Values(result.data.Keys());
    CHECK(loglib::AsStringView(values.at("pid")) == std::string_view{"42"});
    CHECK(loglib::AsStringView(values.at("msg")) == std::string_view{"hello world"});
}

TEST_CASE("Quoted value with C-style escapes [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;

    auto result = loglib::ParseSource(parser, MemorySource("msg=\"a \\\"quoted\\\" word\\nnew line\\ttab\\\\back\"\n"));
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto values = result.data.Lines()[0].Values(result.data.Keys());
    const std::string expected = "a \"quoted\" word\nnew line\ttab\\back";
    CHECK(loglib::AsStringView(values.at("msg")) == std::string_view{expected});
}

TEST_CASE("Unterminated quoted value reports a parse error [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;

    auto result = loglib::ParseSource(parser, MemorySource("key=\"unterminated\n"));
    CHECK(result.errors.size() == 1);
    CHECK(loglib::FormatParseError(result.errors[0]).contains("Unterminated quoted value"));
}

TEST_CASE("Bare key with no '=' is treated as null [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;

    auto result = loglib::ParseSource(parser, MemorySource("flag level=info\n"));
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto values = result.data.Lines()[0].Values(result.data.Keys());
    CHECK(std::holds_alternative<std::monostate>(values.at("flag")));
    CHECK(loglib::AsStringView(values.at("level")) == std::string_view{"info"});
}

TEST_CASE("Repeated keys: last write wins [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;

    auto result = loglib::ParseSource(parser, MemorySource("key=first key=second key=third\n"));
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(
        loglib::AsStringView(result.data.Lines()[0].GetValue(result.data.Keys(), "key")) == std::string_view{"third"}
    );
}

TEST_CASE("Multiple lines parse independently [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;

    auto result = loglib::ParseSource(
        parser,
        MemorySource(
            "level=info msg=\"first line\"\n"
            "level=warn msg=\"second line\" code=42\n"
            "level=error msg=\"third line\"\n"
        )
    );
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 3);

    CHECK(
        loglib::AsStringView(result.data.Lines()[0].GetValue(result.data.Keys(), "level")) == std::string_view{"info"}
    );
    CHECK(
        loglib::AsStringView(result.data.Lines()[1].GetValue(result.data.Keys(), "msg")) ==
        std::string_view{"second line"}
    );
    CHECK(std::get<uint64_t>(result.data.Lines()[1].GetValue(result.data.Keys(), "code")) == 42u);
    CHECK(
        loglib::AsStringView(result.data.Lines()[2].GetValue(result.data.Keys(), "level")) == std::string_view{"error"}
    );
}

TEST_CASE("Blank lines are skipped [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;

    auto result = loglib::ParseSource(parser, MemorySource("\n\nkey=value\n\n"));
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(
        loglib::AsStringView(result.data.Lines()[0].GetValue(result.data.Keys(), "key")) == std::string_view{"value"}
    );
}

TEST_CASE("Last line lacks trailing newline [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;

    auto result = loglib::ParseSource(parser, MemorySource("a=1\nb=2"));
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(std::get<uint64_t>(result.data.Lines()[0].GetValue(result.data.Keys(), "a")) == 1u);
    CHECK(std::get<uint64_t>(result.data.Lines()[1].GetValue(result.data.Keys(), "b")) == 2u);
}

TEST_CASE("Plain text is parsed permissively as null-valued bare keys [logfmt]", "[logfmt_parser]")
{
    // kr/logfmt treats each whitespace-separated word as a
    // (key, null) pair, so plain prose is "valid" logfmt with N
    // null-valued bare keys. We mirror that.
    const loglib::LogfmtParser parser;

    auto result = loglib::ParseSource(parser, MemorySource("level=info\nplain text line\nlevel=error\n"));
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 3);
    CHECK(std::holds_alternative<std::monostate>(result.data.Lines()[1].GetValue(result.data.Keys(), "plain")));
    CHECK(std::holds_alternative<std::monostate>(result.data.Lines()[1].GetValue(result.data.Keys(), "text")));
}

TEST_CASE("Line of only '=' / '\"' surfaces as parse error [logfmt]", "[logfmt_parser]")
{
    // No emitable key/value pairs: the scanner should report the
    // line instead of silently producing an empty record.
    const loglib::LogfmtParser parser;

    auto result = loglib::ParseSource(parser, MemorySource("level=info\n====\nlevel=error\n"));
    REQUIRE(result.errors.size() == 1);
    CHECK(loglib::FormatParseError(result.errors[0]).contains("line 2"));
    CHECK(loglib::FormatParseError(result.errors[0]).contains("Not a logfmt record"));
    REQUIRE(result.data.Lines().size() == 2);
}

TEST_CASE("Parses bare and quoted values [logfmt]", "[logfmt_parser]")
{
    const loglib::LogfmtParser parser;

    auto result = loglib::ParseSource(parser, MemorySource("level=info msg=\"hello world\" code=42 ratio=3.14\n"));
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto &line = result.data.Lines()[0];
    CHECK(loglib::AsStringView(line.GetValue(result.data.Keys(), "level")) == std::string_view{"info"});
    CHECK(loglib::AsStringView(line.GetValue(result.data.Keys(), "msg")) == std::string_view{"hello world"});
    CHECK(std::get<uint64_t>(line.GetValue(result.data.Keys(), "code")) == 42);
    CHECK(std::get<double>(line.GetValue(result.data.Keys(), "ratio")) == 3.14);
}

TEST_CASE("Parse file via FactoryParser auto-detect [logfmt]", "[logfmt_parser]")
{
    const TestLogFile file;
    file.Write("level=info msg=\"hello\"\n");
    auto result = loglib::ParseFile(file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    CHECK(
        loglib::AsStringView(result.data.Lines()[0].GetValue(result.data.Keys(), "msg")) == std::string_view{"hello"}
    );
}

// The `test_common::Logfmt()` writer duplicates `loglib::BareValueIsSafe`
// and `loglib::AppendQuotedString` (intentional, so `test_common` stays
// loglib-free). The two round-trip tests below pin that the duplicate
// stays in sync — otherwise every `[logfmt_parser]` benchmark fixture
// would silently disagree with the parser.
TEST_CASE(
    "test_common::Logfmt() writes round-trip through LogfmtParser (all value families) [logfmt]",
    "[logfmt_parser][round_trip]"
)
{
    using namespace loglib;

    // Fields cover every branch of `AppendLogfmtValue` / typed-bare path:
    // bare-safe / quoted with each C-escape, every numeric/bool family,
    // and null (empty value).
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

    const LogfmtParser parser;
    const ParseResult result = ParseSource(parser, MemorySource(serialized + "\n"));
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto values = result.data.Lines()[0].Values(result.data.Keys());
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
    "test_common::Logfmt() round-trips the generator's record shape end-to-end [logfmt]", "[logfmt_parser][round_trip]"
)
{
    // Drive the actual benchmark fixture through write+parse so a future
    // tweak to `GenerateRandomLogRecord` lands here rather than as a silent
    // benchmark skew.
    using namespace loglib;

    constexpr std::size_t LINE_COUNT = 32;
    constexpr std::uint32_t SEED = 0xBA0BABu;

    // Constant seed: the test asserts byte-equal round-trips.
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp,bugprone-random-generator-seed)
    std::mt19937 rng(SEED);
    std::vector<test_common::LogRecord> records;
    records.reserve(LINE_COUNT);
    for (std::size_t i = 0; i < LINE_COUNT; ++i)
    {
        records.emplace_back(test_common::GenerateRandomLogRecord(rng, i));
    }

    // Move into the fixture and read back via `Records()` so we don't
    // keep two copies of the generated vector alive.
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
        const auto values = result.data.Lines()[i].Values(result.data.Keys());
        const auto &record = fixtureRecords[i];
        REQUIRE(record.is_object());

        // Every key round-trips. Strings compare via `AsStringView`;
        // non-negative `thread_id` maps to `uint64_t` per the bare-value rule.
        CHECK(AsStringView(values.at("timestamp")) == std::string_view{record["timestamp"].get_string()});
        CHECK(AsStringView(values.at("level")) == std::string_view{record["level"].get_string()});
        CHECK(AsStringView(values.at("message")) == std::string_view{record["message"].get_string()});
        CHECK(AsStringView(values.at("component")) == std::string_view{record["component"].get_string()});

        const auto expectedThreadId = static_cast<std::uint64_t>(i % 16);
        CHECK(std::get<std::uint64_t>(values.at("thread_id")) == expectedThreadId);
    }
}

namespace
{

struct CollectingStreamSink final : loglib::LogParseSink
{
    loglib::KeyIndex keys;
    std::vector<loglib::RowBatch> batches;
    bool finished = false;
    bool finishedCancelled = false;

    loglib::KeyIndex &Keys() override
    {
        return keys;
    }
    void OnStarted() override
    {
    }
    void OnBatch(loglib::RowBatch batch) override
    {
        batches.push_back(std::move(batch));
    }
    void OnFinished(bool cancelled) override
    {
        finished = true;
        finishedCancelled = cancelled;
    }
};

std::vector<loglib::LogLine *> FlattenLines(std::vector<loglib::RowBatch> &batches)
{
    std::vector<loglib::LogLine *> lines;
    for (auto &b : batches)
    {
        for (auto &l : b.lines)
        {
            lines.push_back(&l);
        }
    }
    return lines;
}

} // namespace

TEST_CASE(
    "LogfmtParser streaming: indented lines fold into the prior record's last field",
    "[logfmt_parser][stream_line_source]"
)
{
    using namespace loglib;

    const std::string payload = "level=info msg=\"first record\"\n"
                                "level=error msg=\"boom\"\n"
                                "\tgoroutine 1 [running]:\n"
                                "\tmain.main()\n"
                                "\t\tmain.go:42 +0x0\n"
                                "level=info msg=\"after trace\"\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryBytesProducer>(payload));

    CollectingStreamSink sink;
    const LogfmtParser parser;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);
    CHECK_FALSE(sink.finishedCancelled);

    auto lines = FlattenLines(sink.batches);
    REQUIRE(lines.size() == 3);

    const KeyId kLevel = sink.keys.Find("level");
    const KeyId kMsg = sink.keys.Find("msg");
    REQUIRE(kLevel != INVALID_KEY_ID);
    REQUIRE(kMsg != INVALID_KEY_ID);

    // Keep LogValue alive while its string_view is used.
    const LogValue v0 = lines[0]->GetValue(kMsg);
    const LogValue v1 = lines[1]->GetValue(kMsg);
    const LogValue v2 = lines[2]->GetValue(kMsg);
    const auto msg0 = AsStringView(v0);
    const auto msg1 = AsStringView(v1);
    const auto msg2 = AsStringView(v2);
    REQUIRE(msg0.has_value());
    REQUIRE(msg1.has_value());
    REQUIRE(msg2.has_value());

    CHECK(*msg0 == "first record");
    CHECK(
        *msg1 == "boom\n"
                 "\tgoroutine 1 [running]:\n"
                 "\tmain.main()\n"
                 "\t\tmain.go:42 +0x0"
    );
    CHECK(*msg2 == "after trace");

    const std::string raw1 = source.RawLine(lines[1]->LineId());
    CHECK(raw1.contains("level=error"));
    CHECK(raw1.contains("goroutine 1"));
    CHECK(raw1.contains("main.go:42"));
}

TEST_CASE(
    "LogfmtParser streaming: multilineLogfmt=false restores permissive-prose behaviour",
    "[logfmt_parser][stream_line_source]"
)
{
    using namespace loglib;

    const std::string payload = "level=info msg=\"first record\"\n"
                                "\ttab-indented continuation\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryBytesProducer>(payload));

    CollectingStreamSink sink;
    const LogfmtParser parser(false);
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);
    auto lines = FlattenLines(sink.batches);
    REQUIRE(lines.size() == 2);

    const KeyId kTab = sink.keys.Find("tab-indented");
    const KeyId kContinuation = sink.keys.Find("continuation");
    CHECK(kTab != INVALID_KEY_ID);
    CHECK(kContinuation != INVALID_KEY_ID);
}

TEST_CASE(
    "LogfmtParser streaming: orphan continuation before any Emit surfaces a per-line error",
    "[logfmt_parser][stream_line_source]"
)
{
    using namespace loglib;

    const std::string payload = "\torphan indented at start\n"
                                "level=info msg=\"first real record\"\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryBytesProducer>(payload));

    CollectingStreamSink sink;
    const LogfmtParser parser;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);
    auto lines = FlattenLines(sink.batches);
    REQUIRE(lines.size() == 1);

    std::vector<loglib::ParseError> errors;
    for (auto &b : sink.batches)
    {
        errors.insert(errors.end(), b.errors.begin(), b.errors.end());
    }
    REQUIRE(errors.size() == 1);
    CHECK(loglib::FormatParseError(errors[0]).contains("line 1"));
    CHECK(loglib::FormatParseError(errors[0]).contains("Orphaned continuation line"));
}

TEST_CASE(
    "LogfmtParser streaming: consecutive orphan continuations fold into one summary error",
    "[logfmt_parser][stream_line_source]"
)
{
    using namespace loglib;

    const std::string payload = "\torphan 1\n"
                                "\torphan 2\n"
                                "\torphan 3\n"
                                "\torphan 4\n"
                                "\torphan 5\n"
                                "level=info msg=\"first real record\"\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryBytesProducer>(payload));

    CollectingStreamSink sink;
    const LogfmtParser parser;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);
    auto lines = FlattenLines(sink.batches);
    REQUIRE(lines.size() == 1);

    std::vector<loglib::ParseError> errors;
    for (auto &b : sink.batches)
    {
        errors.insert(errors.end(), b.errors.begin(), b.errors.end());
    }
    REQUIRE(errors.size() == 1);
    CHECK(loglib::FormatParseError(errors[0]).contains("lines 1-5"));
    CHECK(loglib::FormatParseError(errors[0]).contains("5 orphaned continuation lines"));
}

TEST_CASE(
    "LogfmtParser streaming: orphan run open at EOF still emits its summary", "[logfmt_parser][stream_line_source]"
)
{
    using namespace loglib;

    const std::string payload = "\torphan 1\n"
                                "\torphan 2\n"
                                "\torphan 3\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryBytesProducer>(payload));

    CollectingStreamSink sink;
    const LogfmtParser parser;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);
    auto lines = FlattenLines(sink.batches);
    REQUIRE(lines.empty());

    std::vector<loglib::ParseError> errors;
    for (auto &b : sink.batches)
    {
        errors.insert(errors.end(), b.errors.begin(), b.errors.end());
    }
    REQUIRE(errors.size() == 1);
    CHECK(loglib::FormatParseError(errors[0]).contains("lines 1-3"));
    CHECK(loglib::FormatParseError(errors[0]).contains("3 orphaned continuation lines"));
}

TEST_CASE("LogfmtParser streaming: final pending record commits on EOF", "[logfmt_parser][stream_line_source]")
{
    using namespace loglib;

    const std::string payload = "level=warn msg=\"trailing\"\n"
                                "\tone continuation line\n"
                                "\tanother one\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryBytesProducer>(payload));

    CollectingStreamSink sink;
    const LogfmtParser parser;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);
    auto lines = FlattenLines(sink.batches);
    REQUIRE(lines.size() == 1);

    const KeyId kMsg = sink.keys.Find("msg");
    REQUIRE(kMsg != INVALID_KEY_ID);
    const LogValue v = lines[0]->GetValue(kMsg);
    const auto msg = AsStringView(v);
    REQUIRE(msg.has_value());
    CHECK(*msg == "trailing\n\tone continuation line\n\tanother one");
}

TEST_CASE(
    "LogfmtParser file: indented continuations fold via ParseFile", "[logfmt_parser][file_line_source][multiline]"
)
{
    using namespace loglib;

    const TestLogFile file{"logfmt_static_multiline.log"};
    file.Write(
        "level=error msg=\"boom\"\n"
        "\tgoroutine 1 [running]:\n"
        "\tmain.main()\n"
        "level=info msg=\"recovered\"\n"
    );

    const LogfmtParser parser;
    const auto result = ParseFile(parser, file.GetFilePath());
    for (const auto &e : result.errors)
    {
        UNSCOPED_INFO("parse error: " << loglib::FormatParseError(e));
    }
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);

    const KeyId kMsg = result.data.Keys().Find("msg");
    REQUIRE(kMsg != INVALID_KEY_ID);
    // Keep LogValue alive while its string_view is used.
    const LogValue v0 = result.data.Lines()[0].GetValue(kMsg);
    const LogValue v1 = result.data.Lines()[1].GetValue(kMsg);
    const auto m0 = AsStringView(v0);
    const auto m1 = AsStringView(v1);
    REQUIRE(m0.has_value());
    REQUIRE(m1.has_value());
    CHECK(m0->contains("boom"));
    CHECK(m0->contains("goroutine 1"));
    CHECK(m0->contains("main.main()"));
    CHECK(*m1 == "recovered");
}

TEST_CASE(
    "LogfmtParser file: cross-batch continuation splices tail record via ParseFile",
    "[logfmt_parser][file_line_source][multiline]"
)
{
    using namespace loglib;

    ParserOptions options;
    options.batchSizeBytes = 24; // Force a cross-batch continuation.

    const TestLogFile file{"logfmt_static_crossbatch.log"};
    file.Write(
        "level=error msg=\"boom\"\n"
        "\tgoroutine 1 [running]:\n"
        "\tmain.main()\n"
        "\t/app/main.go:10 +0x1a\n"
        "level=info msg=\"recovered\"\n"
    );

    auto source = std::make_unique<FileLineSource>(file.GetFilePath());
    FileLineSource *sourcePtr = source.get();
    internal::BufferingSink sink(std::move(source));

    LogfmtParser{}.ParseStreaming(*sourcePtr, sink, options);

    auto data = sink.TakeData();
    const auto errors = sink.TakeErrors();
    for (const auto &e : errors)
    {
        UNSCOPED_INFO("parse error: " << loglib::FormatParseError(e));
    }
    REQUIRE(errors.empty());
    REQUIRE(data.Lines().size() == 2);

    const KeyId kMsg = data.Keys().Find("msg");
    REQUIRE(kMsg != INVALID_KEY_ID);

    const LogValue v0 = data.Lines()[0].GetValue(kMsg);
    const auto m0 = AsStringView(v0);
    REQUIRE(m0.has_value());
    CHECK(m0->contains("boom"));
    CHECK(m0->contains("goroutine 1"));
    CHECK(m0->contains("main.main()"));
    CHECK(m0->contains("/app/main.go:10"));

    const FileLineSource &parsedFile = *sourcePtr;
    REQUIRE(parsedFile.LineCount() == 5);
    CHECK(parsedFile.RawLine(1) == "\tgoroutine 1 [running]:");
    CHECK(parsedFile.RawLine(2) == "\tmain.main()");
    CHECK(parsedFile.RawLine(3) == "\t/app/main.go:10 +0x1a");
    CHECK(parsedFile.RawLine(4) == "level=info msg=\"recovered\"");

    const std::string joined = sourcePtr->RawLine(data.Lines()[0].LineId());
    CHECK(joined.contains("boom"));
    CHECK(joined.contains("main.main()"));
    CHECK_FALSE(joined.contains("recovered"));

    const std::string secondRaw = sourcePtr->RawLine(data.Lines()[1].LineId());
    CHECK(secondRaw == "level=info msg=\"recovered\"");
}

TEST_CASE(
    "LogfmtParser file: blank line inside cross-batch leading continuations stays in the record's span",
    "[logfmt_parser][file_line_source][multiline][cross_batch]"
)
{
    using namespace loglib;

    ParserOptions options;
    options.batchSizeBytes = 24;

    const TestLogFile file{"logfmt_static_crossbatch_blank.log"};
    file.Write(
        "level=error msg=\"boom\"\n"
        "\n"
        "\tgoroutine 1 [running]:\n"
        "level=info msg=\"recovered\"\n"
    );

    auto source = std::make_unique<FileLineSource>(file.GetFilePath());
    FileLineSource *sourcePtr = source.get();
    internal::BufferingSink sink(std::move(source));

    LogfmtParser{}.ParseStreaming(*sourcePtr, sink, options);

    auto data = sink.TakeData();
    const auto errors = sink.TakeErrors();
    for (const auto &e : errors)
    {
        UNSCOPED_INFO("parse error: " << loglib::FormatParseError(e));
    }
    REQUIRE(errors.empty());
    REQUIRE(data.Lines().size() == 2);

    const FileLineSource &parsedFile = *sourcePtr;
    REQUIRE(parsedFile.LineCount() == 4);
    CHECK(parsedFile.RawLine(1).empty());
    CHECK(parsedFile.RawLine(2) == "\tgoroutine 1 [running]:");
    CHECK(parsedFile.RawLine(3) == "level=info msg=\"recovered\"");

    const std::string joined = sourcePtr->RawLine(data.Lines()[0].LineId());
    CHECK(joined.contains("boom"));
    CHECK(joined.contains("goroutine 1"));
    CHECK(joined.contains("\"boom\"\n\n\tgoroutine"));
    CHECK_FALSE(joined.contains("recovered"));

    const KeyId kMsg = data.Keys().Find("msg");
    REQUIRE(kMsg != INVALID_KEY_ID);
    const LogValue v0 = data.Lines()[0].GetValue(kMsg);
    const auto m0 = AsStringView(v0);
    REQUIRE(m0.has_value());
    CHECK(m0->contains("boom"));
    CHECK(m0->contains("goroutine 1"));
}

TEST_CASE(
    "LogfmtParser file: multilineLogfmt=false parses indented lines as bare keys",
    "[logfmt_parser][file_line_source][multiline]"
)
{
    using namespace loglib;

    const TestLogFile file{"logfmt_static_multiline_disabled.log"};
    file.Write(
        "level=info msg=\"first record\"\n"
        "\ttab-indented continuation\n"
    );

    const LogfmtParser parser(false);
    ParserOptions options;

    auto fileSource = std::make_unique<FileLineSource>(file.GetFilePath());
    FileLineSource *sourcePtr = fileSource.get();
    internal::BufferingSink sink(std::move(fileSource));
    parser.ParseStreaming(*sourcePtr, sink, options);

    auto data = sink.TakeData();
    REQUIRE(data.Lines().size() == 2);

    const KeyId kTab = data.Keys().Find("tab-indented");
    const KeyId kContinuation = data.Keys().Find("continuation");
    CHECK(kTab != INVALID_KEY_ID);
    CHECK(kContinuation != INVALID_KEY_ID);
}
