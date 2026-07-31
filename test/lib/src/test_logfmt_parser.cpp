#include "common.hpp"

#include <loglib/bytes_producer.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/internal/buffering_sink.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/logfmt_parser.hpp>
#include <loglib/stream_line_source.hpp>

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
    file.Write(
        "level=info msg=\"first line\"\n"
        "level=warn msg=\"second line\" code=42\n"
        "level=error msg=\"third line\"\n"
    );

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
    const std::string emitted = LogfmtParser::ToString(
        LogMap{
            {"safe", LogValue{std::string_view{"value"}}},
            {"spaced", LogValue{std::string_view{"hello world"}}},
            {"with_quote", LogValue{std::string_view{"a\"b"}}},
            {"with_newline", LogValue{std::string_view{"line1\nline2"}}}
        }
    );

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
        const auto values = result.data.Lines()[i].Values();
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

class InMemoryProducer final : public loglib::BytesProducer
{
public:
    explicit InMemoryProducer(std::string bytes)
        : mBytes(std::move(bytes))
    {
    }

    size_t Read(std::span<char> buffer) override
    {
        if (mCursor >= mBytes.size())
        {
            mClosed = true;
            return 0;
        }
        const size_t available = mBytes.size() - mCursor;
        const size_t n = std::min(available, buffer.size());
        std::memcpy(buffer.data(), mBytes.data() + mCursor, n);
        mCursor += n;
        if (mCursor >= mBytes.size())
        {
            mClosed = true;
        }
        return n;
    }

    void WaitForBytes(std::chrono::milliseconds /*timeout*/) override
    {
    }

    void Stop() noexcept override
    {
        mClosed = true;
    }

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return mClosed;
    }

    [[nodiscard]] std::string DisplayName() const override
    {
        return "in-memory";
    }

private:
    std::string mBytes;
    size_t mCursor = 0;
    bool mClosed = false;
};

struct CollectingStreamSink final : loglib::LogParseSink
{
    loglib::KeyIndex keys;
    std::vector<loglib::StreamedBatch> batches;
    bool finished = false;
    bool finishedCancelled = false;

    loglib::KeyIndex &Keys() override
    {
        return keys;
    }
    void OnStarted() override
    {
    }
    void OnBatch(loglib::StreamedBatch batch) override
    {
        batches.push_back(std::move(batch));
    }
    void OnFinished(bool cancelled) override
    {
        finished = true;
        finishedCancelled = cancelled;
    }
};

std::vector<loglib::LogLine *> FlattenLines(std::vector<loglib::StreamedBatch> &batches)
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
    "LogfmtParser streaming: indented lines fold into the prior record's last field", "[logfmt_parser][stream_line_source]"
)
{
    using namespace loglib;

    const std::string payload =
        "level=info msg=\"first record\"\n"
        "level=error msg=\"boom\"\n"
        "\tgoroutine 1 [running]:\n"
        "\tmain.main()\n"
        "\t\tmain.go:42 +0x0\n"
        "level=info msg=\"after trace\"\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryProducer>(payload));

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
        *msg1
        == "boom\n"
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

TEST_CASE("LogfmtParser streaming: multilineLogfmt=false restores permissive-prose behaviour", "[logfmt_parser][stream_line_source]")
{
    using namespace loglib;

    const std::string payload =
        "level=info msg=\"first record\"\n"
        "\ttab-indented continuation\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryProducer>(payload));

    CollectingStreamSink sink;
    const LogfmtParser parser;
    ParserOptions options;
    options.multilineLogfmt = false;
    parser.ParseStreaming(source, sink, options);

    REQUIRE(sink.finished);
    auto lines = FlattenLines(sink.batches);
    REQUIRE(lines.size() == 2);

    const KeyId kTab = sink.keys.Find("tab-indented");
    const KeyId kContinuation = sink.keys.Find("continuation");
    CHECK(kTab != INVALID_KEY_ID);
    CHECK(kContinuation != INVALID_KEY_ID);
}

TEST_CASE(
    "LogfmtParser streaming: orphan continuation before any Emit surfaces a per-line error", "[logfmt_parser][stream_line_source]"
)
{
    using namespace loglib;

    const std::string payload =
        "\torphan indented at start\n"
        "level=info msg=\"first real record\"\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryProducer>(payload));

    CollectingStreamSink sink;
    const LogfmtParser parser;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);
    auto lines = FlattenLines(sink.batches);
    REQUIRE(lines.size() == 1);

    std::vector<std::string> errors;
    for (auto &b : sink.batches)
    {
        errors.insert(errors.end(), b.errors.begin(), b.errors.end());
    }
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].contains("line 1"));
    CHECK(errors[0].contains("Orphaned continuation line"));
}

TEST_CASE(
    "LogfmtParser streaming: consecutive orphan continuations fold into one summary error",
    "[logfmt_parser][stream_line_source]"
)
{
    using namespace loglib;

    const std::string payload =
        "\torphan 1\n"
        "\torphan 2\n"
        "\torphan 3\n"
        "\torphan 4\n"
        "\torphan 5\n"
        "level=info msg=\"first real record\"\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryProducer>(payload));

    CollectingStreamSink sink;
    const LogfmtParser parser;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);
    auto lines = FlattenLines(sink.batches);
    REQUIRE(lines.size() == 1);

    std::vector<std::string> errors;
    for (auto &b : sink.batches)
    {
        errors.insert(errors.end(), b.errors.begin(), b.errors.end());
    }
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].contains("lines 1-5"));
    CHECK(errors[0].contains("5 orphaned continuation lines"));
}

TEST_CASE(
    "LogfmtParser streaming: orphan run open at EOF still emits its summary",
    "[logfmt_parser][stream_line_source]"
)
{
    using namespace loglib;

    const std::string payload =
        "\torphan 1\n"
        "\torphan 2\n"
        "\torphan 3\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryProducer>(payload));

    CollectingStreamSink sink;
    const LogfmtParser parser;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);
    auto lines = FlattenLines(sink.batches);
    REQUIRE(lines.empty());

    std::vector<std::string> errors;
    for (auto &b : sink.batches)
    {
        errors.insert(errors.end(), b.errors.begin(), b.errors.end());
    }
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].contains("lines 1-3"));
    CHECK(errors[0].contains("3 orphaned continuation lines"));
}

TEST_CASE("LogfmtParser streaming: final pending record commits on EOF", "[logfmt_parser][stream_line_source]")
{
    using namespace loglib;

    const std::string payload =
        "level=warn msg=\"trailing\"\n"
        "\tone continuation line\n"
        "\tanother one\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryProducer>(payload));

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


TEST_CASE("LogfmtParser file: indented continuations fold via ParseFile", "[logfmt_parser][file_line_source][multiline]")
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
        UNSCOPED_INFO("parse error: " << e);
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

    internal::AdvancedParserOptions advanced;
    advanced.batchSizeBytes = 24; // Force a cross-batch continuation.

    const TestLogFile file{"logfmt_static_crossbatch.log"};
    file.Write(
        "level=error msg=\"boom\"\n"
        "\tgoroutine 1 [running]:\n"
        "\tmain.main()\n"
        "\t/app/main.go:10 +0x1a\n"
        "level=info msg=\"recovered\"\n"
    );

    auto logFile = std::make_unique<LogFile>(file.GetFilePath());
    auto source = std::make_unique<FileLineSource>(std::move(logFile));
    FileLineSource *sourcePtr = source.get();
    internal::BufferingSink sink(std::move(source));

    LogfmtParser::ParseStreaming(*sourcePtr, sink, ParserOptions{}, advanced);

    auto data = sink.TakeData();
    const auto errors = sink.TakeErrors();
    for (const auto &e : errors)
    {
        UNSCOPED_INFO("parse error: " << e);
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

    const LogFile &parsedFile = sourcePtr->File();
    REQUIRE(parsedFile.GetLineCount() == 5);
    CHECK(parsedFile.GetLine(1) == "\tgoroutine 1 [running]:");
    CHECK(parsedFile.GetLine(2) == "\tmain.main()");
    CHECK(parsedFile.GetLine(3) == "\t/app/main.go:10 +0x1a");
    CHECK(parsedFile.GetLine(4) == "level=info msg=\"recovered\"");

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

    internal::AdvancedParserOptions advanced;
    advanced.batchSizeBytes = 24;

    const TestLogFile file{"logfmt_static_crossbatch_blank.log"};
    file.Write(
        "level=error msg=\"boom\"\n"
        "\n"
        "\tgoroutine 1 [running]:\n"
        "level=info msg=\"recovered\"\n"
    );

    auto logFile = std::make_unique<LogFile>(file.GetFilePath());
    auto source = std::make_unique<FileLineSource>(std::move(logFile));
    FileLineSource *sourcePtr = source.get();
    internal::BufferingSink sink(std::move(source));

    LogfmtParser::ParseStreaming(*sourcePtr, sink, ParserOptions{}, advanced);

    auto data = sink.TakeData();
    const auto errors = sink.TakeErrors();
    for (const auto &e : errors)
    {
        UNSCOPED_INFO("parse error: " << e);
    }
    REQUIRE(errors.empty());
    REQUIRE(data.Lines().size() == 2);

    const LogFile &parsedFile = sourcePtr->File();
    REQUIRE(parsedFile.GetLineCount() == 4);
    CHECK(parsedFile.GetLine(1).empty());
    CHECK(parsedFile.GetLine(2) == "\tgoroutine 1 [running]:");
    CHECK(parsedFile.GetLine(3) == "level=info msg=\"recovered\"");

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

    const LogfmtParser parser;
    ParserOptions options;
    options.multilineLogfmt = false;

    auto logFile = std::make_unique<LogFile>(file.GetFilePath());
    auto fileSource = std::make_unique<FileLineSource>(std::move(logFile));
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
