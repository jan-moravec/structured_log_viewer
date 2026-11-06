#include "common.hpp"

#include <loglib/core/bytes_producer.hpp>
#include <loglib/core/file_line_source.hpp>
#include <loglib/core/log_data.hpp>
#include <loglib/core/log_file.hpp>
#include <loglib/core/stream_line_source.hpp>
#include "parse_only.hpp"
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/parse/parsers/regex_parser.hpp>
#include <loglib/parse/regex_templates.hpp>
#include <loglib/schema/log_configuration.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace loglib;

TEST_CASE("RegexParser validates against built-in templates [regex]", "[regex_parser]")
{
    const RegexParser parser{std::string{}};
    const TestLogFile file("regex_isvalid.log");
    // Two syslog lines: enough to trip `IS_VALID_MIN_MATCHES = 2`
    // and identify Syslog (RFC3164).
    file.Write(
        "Apr 28 04:02:03 host-a systemd: System starting\n"
        "Jun 27 01:47:20 host-b configd[17]: network changed\n"
    );
    CHECK(parser.IsValid(file.GetFilePath()));

    const auto detected = DetectRegexTemplate(file.GetFilePath());
    REQUIRE(detected.has_value());
    CHECK(detected->name == "Syslog (RFC3164)");
}

TEST_CASE("RegexParser rejects single-line files [regex]", "[regex_parser]")
{
    // One matched line is too brittle for auto-detect; the probe
    // requires at least two non-blank lines.
    const RegexParser parser{std::string{}};
    const TestLogFile file("regex_oneline.log");
    file.Write("Apr 28 04:02:03 host-a systemd: System starting\n");
    CHECK_FALSE(parser.IsValid(file.GetFilePath()));
}

TEST_CASE("RegexParser rejects JSON / logfmt files [regex]", "[regex_parser]")
{
    // Auto-detect precedence: a file another probe claims must not
    // also be claimed by Regex. JSON is the broadest non-regex shape.
    const RegexParser parser{std::string{}};
    const TestLogFile file("regex_rejects_json.log");
    file.Write(
        R"({"level":"info","msg":"hello"})"
        "\n"
        R"({"level":"warn","msg":"world"})"
        "\n"
    );
    CHECK_FALSE(parser.IsValid(file.GetFilePath()));
}

TEST_CASE("RegexParser with an empty pattern surfaces one error [regex]", "[regex_parser]")
{
    // A RegexParser with an empty pattern fails closed.
    // Parsing must not crash; it surfaces one error and ends cleanly.
    const RegexParser parser{std::string{}};
    const TestLogFile file("regex_no_pattern.log");
    file.Write(
        "Apr 28 04:02:03 host-a systemd: System starting\n"
        "Apr 28 04:02:04 host-a systemd: another line\n"
    );

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.size() == 1);
    CHECK(loglib::FormatParseError(result.errors[0]).contains("non-empty pattern"));
    CHECK(result.data.Lines().empty());
}

TEST_CASE("RegexParser unparsable pattern surfaces error [regex]", "[regex_parser]")
{
    const RegexParser parser(R"((?<a)"); // dangling group
    const TestLogFile file("regex_bad_pattern.log");
    file.Write("anything\nat all\n");

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.size() == 1);
    CHECK(loglib::FormatParseError(result.errors[0]).contains("Pattern compile failed"));
    CHECK(result.data.Lines().empty());
}

TEST_CASE("RegexParser pattern without named groups surfaces error [regex]", "[regex_parser]")
{
    // Anonymous groups don't map to columns; refuse rather than
    // producing schemaless rows.
    const RegexParser parser(R"(^(\w+)\s+(.*)$)");
    const TestLogFile file("regex_no_groups.log");
    file.Write("a b\nc d\n");

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.size() == 1);
    CHECK(loglib::FormatParseError(result.errors[0]).contains("named capture groups"));
    CHECK(result.data.Lines().empty());
}

TEST_CASE("RegexParser parses well-formed lines [regex]", "[regex_parser]")
{
    // Simple `LEVEL message` shape; named groups map to columns.
    const RegexParser parser(R"(^(?<level>\w+)\s+(?<message>.*)$)");
    const TestLogFile file("regex_parse_minimal.log");
    file.Write("info hello\nerror boom\n");

    auto result = ParseFile(parser, file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(AsStringView(result.data.Lines()[0].GetValue("level")) == std::string_view{"info"});
    CHECK(AsStringView(result.data.Lines()[0].GetValue("message")) == std::string_view{"hello"});
    CHECK(AsStringView(result.data.Lines()[1].GetValue("level")) == std::string_view{"error"});
    CHECK(AsStringView(result.data.Lines()[1].GetValue("message")) == std::string_view{"boom"});
}

TEST_CASE("RegexParser types numeric captures [regex]", "[regex_parser]")
{
    // `ClassifyBareScalar` promotes numeric / bool captures the
    // same way it does for CSV / logfmt bare cells.
    const RegexParser parser(R"(^(?<level>\w+)\s+(?<code>\d+)\s+(?<ratio>\S+)\s+(?<ok>\S+)$)");
    const TestLogFile file("regex_typing.log");
    file.Write(
        "info 200 0.75 true\n"
        "warn 404 -1.5 false\n"
    );

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);

    const auto &row0 = result.data.Lines()[0];
    CHECK(std::get<std::uint64_t>(row0.GetValue("code")) == 200U);
    CHECK(std::get<double>(row0.GetValue("ratio")) == Catch::Approx(0.75));
    CHECK(std::get<bool>(row0.GetValue("ok")) == true);

    const auto &row1 = result.data.Lines()[1];
    CHECK(std::get<std::uint64_t>(row1.GetValue("code")) == 404U);
    CHECK(std::get<double>(row1.GetValue("ratio")) == Catch::Approx(-1.5));
    CHECK(std::get<bool>(row1.GetValue("ok")) == false);
}

TEST_CASE("RegexParser non-matching lines surface as errors [regex]", "[regex_parser]")
{
    // Non-match is per-line; the rest of the file still parses.
    const RegexParser parser(R"(^(?<level>\w+):(?<message>.+)$)");
    const TestLogFile file("regex_non_matching.log");
    file.Write(
        "info:hello\n"
        "this line does not match\n"
        "error:boom\n"
    );

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.data.Lines().size() == 2);
    REQUIRE(result.errors.size() == 1);
    CHECK(loglib::FormatParseError(result.errors[0]).contains("line 2"));
    CHECK(loglib::FormatParseError(result.errors[0]).contains("did not match"));
}

TEST_CASE("RegexParser optional unmatched groups -> monostate [regex]", "[regex_parser]")
{
    // `pid` is optional: absent on line 1, present on line 2.
    // Captures that didn't participate in the match drop to
    // monostate (not the empty string), matching CSV's "missing
    // trailing cell" behaviour.
    const RegexParser parser(R"(^(?<program>\w+)(?:\[(?<pid>\d+)\])?:\s+(?<message>.*)$)");
    const TestLogFile file("regex_optional.log");
    file.Write(
        "systemd: System starting\n"
        "configd[17]: network changed\n"
    );

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);

    const auto &row0 = result.data.Lines()[0];
    CHECK(AsStringView(row0.GetValue("program")) == std::string_view{"systemd"});
    CHECK(std::holds_alternative<std::monostate>(row0.GetValue("pid")));
    CHECK_FALSE(row0.Values().contains("pid"));

    const auto &row1 = result.data.Lines()[1];
    CHECK(AsStringView(row1.GetValue("program")) == std::string_view{"configd"});
    CHECK(std::get<std::uint64_t>(row1.GetValue("pid")) == 17U);
}

TEST_CASE("RegexParser auto-detect through ParseFile picks the matched template [regex]", "[regex_parser]")
{
    // End-to-end: `loglib::ParseFile(path)` runs the full auto-
    // detect loop, including the Regex special case. Two syslog-
    // shaped lines with no header.
    const TestLogFile file("regex_e2e.log");
    file.Write(
        "Apr 28 04:02:03 host-a systemd: System starting\n"
        "Apr 28 04:02:04 host-b CRON[1234]: (root) CMD (test)\n"
    );

    auto result = ParseFile(file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    // Columns from the syslog template.
    CHECK(AsStringView(result.data.Lines()[0].GetValue("hostname")) == std::string_view{"host-a"});
    CHECK(AsStringView(result.data.Lines()[0].GetValue("program")) == std::string_view{"systemd"});
    CHECK(AsStringView(result.data.Lines()[1].GetValue("hostname")) == std::string_view{"host-b"});
    CHECK(AsStringView(result.data.Lines()[1].GetValue("program")) == std::string_view{"CRON"});
    CHECK(std::get<std::uint64_t>(result.data.Lines()[1].GetValue("pid")) == 1234U);
}

TEST_CASE("RegexParser does not steal JSON / CSV files [regex]", "[regex_parser]")
{
    // Regression: auto-detect order is JSON to logfmt to CSV to
    // Regex. A two-line JSON file must come out as JSON — Regex
    // is the *last* fallback, so we'd see syslog-style columns
    // if the order ever drifted.
    const TestLogFile file("regex_precedence.log");
    file.Write(
        R"({"level":"info","msg":"hello"})"
        "\n"
        R"({"level":"warn","msg":"world"})"
        "\n"
    );

    auto result = ParseFile(file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    // If Regex had won, there'd be no `level` JSON column.
    CHECK(AsStringView(result.data.Lines()[0].GetValue("level")) == std::string_view{"info"});
}

TEST_CASE("RegexParser captures named groups as values [regex]", "[regex_parser]")
{
    const RegexParser parser(R"(^(?<level>\w+)\s+(?<message>.*)$)");
    const TestLogFile file("regex_tostring.log");
    file.Write("info hello world\n");

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const auto &line = result.data.Lines()[0];
    CHECK(AsStringView(line.GetValue("level")) == std::string_view{"info"});
    CHECK(AsStringView(line.GetValue("message")) == std::string_view{"hello world"});
}

TEST_CASE("RegexParser auto-detect and parse handle a UTF-8 BOM [regex]", "[regex_parser]")
{
    // Some editors (Notepad, older PowerShell) prepend a UTF-8 BOM
    // to log files. Without the BOM strip the `^date` anchor in
    // every built-in template fails at byte 0 of line 1 and both
    // auto-detect and parse silently refuse the file. After the
    // fix `DetectRegexTemplate` claims the file *and* the first
    // line emits a row (not a `did not match` error).
    const TestLogFile file("regex_bom.log");
    file.Write(
        "\xEF\xBB\xBF"
        "Apr 28 04:02:03 host-a systemd: System starting\n"
        "Apr 28 04:02:04 host-a systemd: another line\n"
    );

    const auto detected = DetectRegexTemplate(file.GetFilePath());
    REQUIRE(detected.has_value());
    CHECK(detected->name == "Syslog (RFC3164)");

    auto result = ParseFile(file.GetFilePath());
    CHECK(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);
    CHECK(AsStringView(result.data.Lines()[0].GetValue("hostname")) == std::string_view{"host-a"});
}

TEST_CASE("RegexParser surfaces columns in pattern-source order [regex]", "[regex_parser]")
{
    // PCRE2's name table returns groups alphabetically, but
    // `LogTable`'s column order follows `KeyIndex` allocation
    // order — so we deliberately intern named groups by their
    // pattern-source index. A pattern with `message` declared
    // before `level` must intern `message` first, not the
    // alphabetical permutation.
    const RegexParser parser(R"(^(?<message>[^|]*)\|(?<level>\w+)$)");
    const TestLogFile file("regex_tostring_order.log");
    file.Write("hello world|info\n");

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const KeyId messageKey = result.data.Keys().Find("message");
    const KeyId levelKey = result.data.Keys().Find("level");
    REQUIRE(messageKey != INVALID_KEY_ID);
    REQUIRE(levelKey != INVALID_KEY_ID);
    CHECK(messageKey < levelKey);
}

namespace
{

/// `LogParseSink` that records every emitted batch verbatim so
/// the test can assert on `newKeys`. Owns its `KeyIndex` so the
/// streaming parser interns into a sink-local index rather than
/// mutating a shared one.
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

} // namespace

// The pattern's named capture groups are interned into `KeyIndex` before
// `RunStreamingParseLoop` starts. The parser passes them as declared keys, so
// the first batch announces them in `newKeys` and `LogTable::AppendBatch`
// creates the columns. Without that, streaming regex sessions would ingest
// rows with no visible columns.
TEST_CASE(
    "RegexParser streaming surfaces named-group columns via newKeys [regex][stream_line_source]", "[regex_parser]"
)
{
    using namespace loglib;

    const RegexParser parser(R"(^(?<level>\w+)\s+(?<message>.*)$)");

    const std::string payload = "info hello\n"
                                "warn world\n"
                                "error boom\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryBytesProducer>(payload));

    CollectingStreamSink sink;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);
    CHECK_FALSE(sink.finishedCancelled);

    // `level` and `message` must appear in the union of `newKeys`
    // across all emitted batches; without the baseline fix both
    // lists would be empty.
    std::vector<std::string> announcedKeys;
    for (const auto &batch : sink.batches)
    {
        for (const auto &key : batch.newKeys)
        {
            announcedKeys.push_back(key);
        }
    }
    CHECK(std::ranges::find(announcedKeys, "level") != announcedKeys.end());
    CHECK(std::ranges::find(announcedKeys, "message") != announcedKeys.end());

    // Sanity: rows carry the captured values, showing the
    // regression is in new-key surfacing rather than parsing.
    size_t totalLines = 0;
    for (const auto &batch : sink.batches)
    {
        totalLines += batch.lines.size();
    }
    CHECK(totalLines == 3);
}

namespace
{

/// Aggregate the per-batch errors a `CollectingStreamSink`
/// captured into one flat list so the error-path streaming tests
/// below can assert on the message the parser produced without
/// hard-coding a batch index. The failure mode a caller cares
/// about is "did the streaming pipeline surface this error at
/// all" — the batch boundary the coalescer picked is an
/// implementation detail.
std::vector<loglib::ParseError> FlattenSinkErrors(const CollectingStreamSink &sink)
{
    std::vector<loglib::ParseError> all;
    for (const auto &batch : sink.batches)
    {
        for (const auto &err : batch.errors)
        {
            all.push_back(err);
        }
    }
    return all;
}

/// Drive `RegexParser::ParseStreaming(StreamLineSource, ...)`
/// with @p pattern and a two-line payload, then return the
/// collected errors. Wraps the boilerplate the three streaming
/// error tests need so each test case reads as a single assertion
/// block. The payload text is irrelevant on the error paths —
/// they fail before any line is decoded — but keeping it varied
/// makes it obvious in stack traces which test tripped.
std::vector<loglib::ParseError> RunStreamingParseCollectingErrors(std::string_view pattern, std::string_view payload)
{
    const RegexParser parser{std::string{pattern}};
    StreamLineSource source(
        std::filesystem::path("regex_stream_error.log"), std::make_unique<InMemoryBytesProducer>(std::string{payload})
    );
    CollectingStreamSink sink;
    parser.ParseStreaming(source, sink, ParserOptions{});
    REQUIRE(sink.finished);
    return FlattenSinkErrors(sink);
}

} // namespace

TEST_CASE("RegexParser streaming with empty pattern surfaces error [regex][stream_line_source]", "[regex_parser]")
{
    // Streaming counterpart of the static-path "default-constructed
    // parse without pattern surfaces error" case. The default ctor
    // means "read pattern from options"; without a
    // `LogConfiguration` on the options the resolved pattern is
    // empty and the parser must fail closed before decoding any
    // line. The streaming overload reports this error through
    // `EmitErrorAndFinish` with the streaming flush thresholds.
    const RegexParser parser{std::string{}};
    StreamLineSource source(
        std::filesystem::path("regex_stream_empty_pattern.log"), std::make_unique<InMemoryBytesProducer>("one\ntwo\n")
    );
    CollectingStreamSink sink;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);
    CHECK_FALSE(sink.finishedCancelled);
    const auto errors = FlattenSinkErrors(sink);
    REQUIRE(errors.size() == 1);
    CHECK(loglib::FormatParseError(errors[0]).contains("non-empty pattern"));
}

TEST_CASE("RegexParser streaming with unparsable pattern surfaces error [regex][stream_line_source]", "[regex_parser]")
{
    // Dangling group `(?<a` fails PCRE2 compile; the compile error
    // must be forwarded through the streaming sink verbatim
    // (fmt-formatted "Pattern compile failed at offset ..." from
    // `CompiledPattern::Compile`) rather than crashing the
    // pipeline or eating the message.
    const auto errors = RunStreamingParseCollectingErrors(R"((?<a)", "one\ntwo\n");
    REQUIRE(errors.size() == 1);
    CHECK(loglib::FormatParseError(errors[0]).contains("Pattern compile failed"));
}

TEST_CASE(
    "RegexParser streaming with anonymous groups only surfaces error [regex][stream_line_source]", "[regex_parser]"
)
{
    // Anonymous groups don't map to columns. Same guarantee the
    // static path already exercises: refuse before running the
    // decoder so callers see a clear error rather than a stream
    // of "did not match" per line (which is what would happen if
    // the guard was ever removed).
    const auto errors = RunStreamingParseCollectingErrors(R"(^(\w+)\s+(.*)$)", "info hello\nwarn world\n");
    REQUIRE(errors.size() == 1);
    CHECK(loglib::FormatParseError(errors[0]).contains("named capture groups"));
}

TEST_CASE(
    "RegexParser streaming: indented continuation folds into the last named group",
    "[regex_parser][stream_line_source][multiline]"
)
{
    using namespace loglib;

    const std::string pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    const RegexTemplate extra{
        .name = "test-indented-multiline",
        .pattern = pattern,
        .sampleLines = {"info hello"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::Indented,
        .headerAnchor = "",
    };

    const RegexParser parser{extra.pattern, extra.continuationMode, extra.headerAnchor};

    const std::string payload = "error boom\n"
                                "\tat com.example.Foo.bar(Foo.java:42)\n"
                                "\tat com.example.Baz.qux(Baz.java:7)\n"
                                "info recovered\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryBytesProducer>(payload));
    CollectingStreamSink sink;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);
    CHECK_FALSE(sink.finishedCancelled);

    std::vector<LogLine *> lines;
    for (auto &b : sink.batches)
    {
        for (auto &l : b.lines)
        {
            lines.push_back(&l);
        }
    }
    REQUIRE(lines.size() == 2);

    const KeyId kMessage = sink.keys.Find("message");
    REQUIRE(kMessage != INVALID_KEY_ID);
    const LogValue v0 = lines[0]->GetValue(kMessage);
    const LogValue v1 = lines[1]->GetValue(kMessage);
    const auto m0 = AsStringView(v0);
    const auto m1 = AsStringView(v1);
    REQUIRE(m0.has_value());
    REQUIRE(m1.has_value());

    CHECK(
        *m0 == "boom\n"
               "\tat com.example.Foo.bar(Foo.java:42)\n"
               "\tat com.example.Baz.qux(Baz.java:7)"
    );
    CHECK(*m1 == "recovered");

    const std::string raw0 = source.RawLine(lines[0]->LineId());
    CHECK(raw0.contains("error boom"));
    CHECK(raw0.contains("Foo.java:42"));
    CHECK(raw0.contains("Baz.java:7"));
}

TEST_CASE(
    "RegexParser streaming: blank line between header and continuation stays in RawLine, not in field",
    "[regex_parser][stream_line_source][multiline]"
)
{
    using namespace loglib;

    const std::string pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    const RegexTemplate extra{
        .name = "test-stream-multiline-blank",
        .pattern = pattern,
        .sampleLines = {"info hello"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::Indented,
        .headerAnchor = "",
    };

    const RegexParser parser{extra.pattern, extra.continuationMode, extra.headerAnchor};

    const std::string payload = "error boom\n"
                                "\n"
                                "\tat com.example.Foo.bar(Foo.java:42)\n"
                                "\n"
                                "info recovered\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryBytesProducer>(payload));
    CollectingStreamSink sink;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);

    std::vector<LogLine *> lines;
    for (auto &b : sink.batches)
    {
        for (auto &l : b.lines)
        {
            lines.push_back(&l);
        }
    }
    REQUIRE(lines.size() == 2);

    const KeyId kMessage = sink.keys.Find("message");
    REQUIRE(kMessage != INVALID_KEY_ID);
    const LogValue v0 = lines[0]->GetValue(kMessage);
    const auto m0 = AsStringView(v0);
    REQUIRE(m0.has_value());
    CHECK(*m0 == "boom\n\tat com.example.Foo.bar(Foo.java:42)");

    const std::string raw0 = source.RawLine(lines[0]->LineId());
    CHECK(raw0.contains("boom\n\n\tat"));
    CHECK_FALSE(raw0.contains("info recovered"));
    CHECK_FALSE(raw0.ends_with('\n'));

    const std::string raw1 = source.RawLine(lines[1]->LineId());
    CHECK(raw1 == "info recovered");
}

TEST_CASE(
    "RegexParser streaming: UntilNextHeader folds non-matching lines into the last named group",
    "[regex_parser][stream_line_source][multiline]"
)
{
    using namespace loglib;

    const std::string pattern = R"(^\[(?<level>\w+)\]\s+(?<message>.*)$)";
    const RegexTemplate extra{
        .name = "test-header-multiline",
        .pattern = pattern,
        .sampleLines = {"[ERROR] boom"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::UntilNextHeader,
        .headerAnchor = "",
    };

    const RegexParser parser{extra.pattern, extra.continuationMode, extra.headerAnchor};

    const std::string payload = "[ERROR] boom\n"
                                "Traceback (most recent call last):\n"
                                "During handling of the above exception, another exception occurred:\n"
                                "  File \"a.py\", line 7\n"
                                "[INFO] recovered\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryBytesProducer>(payload));
    CollectingStreamSink sink;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);

    std::vector<LogLine *> lines;
    for (auto &b : sink.batches)
    {
        for (auto &l : b.lines)
        {
            lines.push_back(&l);
        }
    }
    REQUIRE(lines.size() == 2);

    const KeyId kMessage = sink.keys.Find("message");
    REQUIRE(kMessage != INVALID_KEY_ID);
    const LogValue v0 = lines[0]->GetValue(kMessage);
    const auto m0 = AsStringView(v0);
    REQUIRE(m0.has_value());
    CHECK(m0->contains("Traceback"));
    CHECK(m0->contains("During handling"));
    CHECK(m0->contains("a.py"));
}

TEST_CASE(
    "RegexParser streaming: template without continuationMode keeps shipped single-line behaviour",
    "[regex_parser][stream_line_source][multiline]"
)
{
    using namespace loglib;

    const std::string pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    const RegexTemplate extra{
        .name = "test-legacy-singleline",
        .pattern = pattern,
        .sampleLines = {"info hello"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::None,
        .headerAnchor = "",
    };

    const RegexParser parser{extra.pattern, extra.continuationMode, extra.headerAnchor};

    const std::string payload = "error boom\n"
                                "\tat com.example.Foo.bar(Foo.java:42)\n"
                                "info recovered\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<InMemoryBytesProducer>(payload));
    CollectingStreamSink sink;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);

    std::vector<LogLine *> lines;
    std::vector<loglib::ParseError> errors;
    for (auto &b : sink.batches)
    {
        for (auto &l : b.lines)
        {
            lines.push_back(&l);
        }
        errors.insert(errors.end(), b.errors.begin(), b.errors.end());
    }
    CHECK(lines.size() == 2);
    CHECK(errors.size() == 1);
}

TEST_CASE(
    "RegexParser file: indented continuations fold into the last named group via ParseFile",
    "[regex_parser][file_line_source][multiline]"
)
{
    using namespace loglib;

    const std::string pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    const RegexTemplate extra{
        .name = "test-static-indented",
        .pattern = pattern,
        .sampleLines = {"info hello"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::Indented,
        .headerAnchor = "",
    };

    const RegexParser parser{extra.pattern, extra.continuationMode, extra.headerAnchor};

    const TestLogFile file("regex_static_multiline.log");
    file.Write(
        "error boom\n"
        "\tat com.example.Foo.bar(Foo.java:42)\n"
        "\tat com.example.Baz.qux(Baz.java:7)\n"
        "info recovered\n"
    );

    auto result = ParseFile(parser, file.GetFilePath());
    for (const auto &e : result.errors)
    {
        UNSCOPED_INFO("parse error: " << loglib::FormatParseError(e));
    }
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);

    const KeyId kMessage = result.data.Keys().Find("message");
    REQUIRE(kMessage != INVALID_KEY_ID);

    const auto &lines = result.data.Lines();
    const LogValue v0 = lines[0].GetValue(kMessage);
    const LogValue v1 = lines[1].GetValue(kMessage);
    const auto m0 = AsStringView(v0);
    const auto m1 = AsStringView(v1);
    REQUIRE(m0.has_value());
    REQUIRE(m1.has_value());
    CHECK(
        *m0 == "boom\n"
               "\tat com.example.Foo.bar(Foo.java:42)\n"
               "\tat com.example.Baz.qux(Baz.java:7)"
    );
    CHECK(*m1 == "recovered");
}

TEST_CASE(
    "RegexParser file: cross-batch continuation splices tail record via ParseFile",
    "[regex_parser][file_line_source][multiline]"
)
{
    using namespace loglib;

    const std::string pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    const RegexTemplate extra{
        .name = "test-static-crossbatch",
        .pattern = pattern,
        .sampleLines = {"info hello"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::Indented,
        .headerAnchor = "",
    };

    ParserOptions options;
    options.batchSizeBytes = 32; // Force a cross-batch continuation.

    const TestLogFile file("regex_static_crossbatch.log");
    file.Write(
        "error boom\n"
        "\tat com.example.Foo.bar(Foo.java:42)\n"
        "\tat com.example.Baz.qux(Baz.java:7)\n"
        "\tat com.example.Wibble.wobble(Wibble.java:99)\n"
        "info recovered\n"
    );

    auto logFile = std::make_unique<LogFile>(file.GetFilePath());
    auto source = std::make_unique<FileLineSource>(std::move(logFile));
    FileLineSource *sourcePtr = source.get();
    internal::BufferingSink sink(std::move(source));

    RegexParser{extra.pattern, extra.continuationMode, extra.headerAnchor}.ParseStreaming(*sourcePtr, sink, options);

    auto data = sink.TakeData();
    const auto errors = sink.TakeErrors();
    for (const auto &e : errors)
    {
        UNSCOPED_INFO("parse error: " << loglib::FormatParseError(e));
    }
    REQUIRE(errors.empty());
    REQUIRE(data.Lines().size() == 2);

    const KeyId kMessage = data.Keys().Find("message");
    REQUIRE(kMessage != INVALID_KEY_ID);

    const LogValue v0 = data.Lines()[0].GetValue(kMessage);
    const auto m0 = AsStringView(v0);
    REQUIRE(m0.has_value());
    CHECK(m0->contains("boom"));
    CHECK(m0->contains("Foo.java:42"));
    CHECK(m0->contains("Baz.java:7"));
    CHECK(m0->contains("Wibble.java:99"));

    const LogFile &parsedFile = sourcePtr->File();
    REQUIRE(parsedFile.GetLineCount() == 5);
    CHECK(parsedFile.GetLine(1) == "\tat com.example.Foo.bar(Foo.java:42)");
    CHECK(parsedFile.GetLine(2) == "\tat com.example.Baz.qux(Baz.java:7)");
    CHECK(parsedFile.GetLine(3) == "\tat com.example.Wibble.wobble(Wibble.java:99)");
    CHECK(parsedFile.GetLine(4) == "info recovered");

    const std::string joined = sourcePtr->RawLine(data.Lines()[0].LineId());
    CHECK(joined.contains("error boom"));
    CHECK(joined.contains("Foo.java:42"));
    CHECK(joined.contains("Wibble.java:99"));
    CHECK_FALSE(joined.contains("info recovered"));

    const std::string secondRaw = sourcePtr->RawLine(data.Lines()[1].LineId());
    CHECK(secondRaw == "info recovered");
}

TEST_CASE(
    "RegexParser file: blank line inside cross-batch leading continuations stays in the record's span",
    "[regex_parser][file_line_source][multiline][cross_batch]"
)
{
    using namespace loglib;

    const std::string pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    const RegexTemplate extra{
        .name = "test-crossbatch-blank-leading",
        .pattern = pattern,
        .sampleLines = {"info hello"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::Indented,
        .headerAnchor = "",
    };

    ParserOptions options;
    options.batchSizeBytes = 16;

    const TestLogFile file("regex_static_crossbatch_blank.log");
    file.Write(
        "error boom\n"
        "\n"
        "\tat com.example.Foo.bar(Foo.java:42)\n"
        "info recovered\n"
    );

    auto logFile = std::make_unique<LogFile>(file.GetFilePath());
    auto source = std::make_unique<FileLineSource>(std::move(logFile));
    FileLineSource *sourcePtr = source.get();
    internal::BufferingSink sink(std::move(source));

    RegexParser{extra.pattern, extra.continuationMode, extra.headerAnchor}.ParseStreaming(*sourcePtr, sink, options);

    auto data = sink.TakeData();
    const auto errors = sink.TakeErrors();
    for (const auto &e : errors)
    {
        UNSCOPED_INFO("parse error: " << loglib::FormatParseError(e));
    }
    REQUIRE(errors.empty());
    REQUIRE(data.Lines().size() == 2);

    const LogFile &parsedFile = sourcePtr->File();
    REQUIRE(parsedFile.GetLineCount() == 4);
    CHECK(parsedFile.GetLine(1).empty());
    CHECK(parsedFile.GetLine(2) == "\tat com.example.Foo.bar(Foo.java:42)");
    CHECK(parsedFile.GetLine(3) == "info recovered");

    const std::string joined = sourcePtr->RawLine(data.Lines()[0].LineId());
    CHECK(joined.contains("error boom"));
    CHECK(joined.contains("Foo.java:42"));
    CHECK(joined.contains("boom\n\n\tat"));
    CHECK_FALSE(joined.contains("info recovered"));

    const KeyId kMessage = data.Keys().Find("message");
    REQUIRE(kMessage != INVALID_KEY_ID);
    const LogValue v0 = data.Lines()[0].GetValue(kMessage);
    const auto m0 = AsStringView(v0);
    REQUIRE(m0.has_value());
    CHECK(m0->contains("boom"));
    CHECK(m0->contains("Foo.java:42"));
}

TEST_CASE(
    "RegexParser file: leading blanks followed by a fresh header are not attached to the held tail",
    "[regex_parser][file_line_source][multiline][cross_batch]"
)
{
    using namespace loglib;

    const std::string pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    const RegexTemplate extra{
        .name = "test-crossbatch-blank-no-cont",
        .pattern = pattern,
        .sampleLines = {"info hello"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::Indented,
        .headerAnchor = "",
    };

    ParserOptions options;
    options.batchSizeBytes = 16;

    const TestLogFile file("regex_static_crossbatch_blank_no_cont.log");
    file.Write(
        "error boom\n"
        "\n"
        "info recovered\n"
    );

    auto logFile = std::make_unique<LogFile>(file.GetFilePath());
    auto source = std::make_unique<FileLineSource>(std::move(logFile));
    FileLineSource *sourcePtr = source.get();
    internal::BufferingSink sink(std::move(source));

    RegexParser{extra.pattern, extra.continuationMode, extra.headerAnchor}.ParseStreaming(*sourcePtr, sink, options);

    auto data = sink.TakeData();
    const auto errors = sink.TakeErrors();
    REQUIRE(errors.empty());
    REQUIRE(data.Lines().size() == 2);

    const std::string firstRaw = sourcePtr->RawLine(data.Lines()[0].LineId());
    CHECK(firstRaw == "error boom");
    const std::string secondRaw = sourcePtr->RawLine(data.Lines()[1].LineId());
    CHECK(secondRaw == "info recovered");
}

TEST_CASE("ValidateRegexPattern rejects empty pattern [regex]", "[regex_parser]")
{
    // GUI pre-flight: the Network Stream dialog calls this before
    // wiring up a live tail so the user sees the "non-empty
    // pattern" error next to the field, not on the first inbound
    // byte. Message must match the parser's runtime error so both
    // surfaces read identically.
    std::string err;
    CHECK_FALSE(ValidateRegexPattern("", err));
    CHECK(err.contains("non-empty pattern"));
}

TEST_CASE("ValidateRegexPattern rejects patterns that fail to compile [regex]", "[regex_parser]")
{
    // A dangling `(?<a` is the simplest PCRE2 syntax error that
    // fits on one line; the surfaced message must include the
    // "Pattern compile failed" prefix `CompiledPattern::Compile`
    // produces so error text stays uniform across the parse-time
    // and pre-flight surfaces.
    std::string err;
    CHECK_FALSE(ValidateRegexPattern(R"((?<a)", err));
    CHECK(err.contains("Pattern compile failed"));
}

TEST_CASE("ValidateRegexPattern rejects patterns without named groups [regex]", "[regex_parser]")
{
    // Anonymous groups don't map to columns; ValidateRegexPattern
    // uses the same "no named groups" guard the parser does. The
    // editor blocks Save on this before the user gets to run the
    // template through a real file.
    std::string err;
    CHECK_FALSE(ValidateRegexPattern(R"(^(\w+)\s+(.*)$)", err));
    CHECK(err.contains("named capture groups"));
}

TEST_CASE("ValidateRegexPattern accepts valid patterns [regex]", "[regex_parser]")
{
    // Happy path: valid PCRE2 with at least one `(?<Name>...)` -
    // the pre-flight check must clear @p errorOut so callers can
    // key their UI off "empty error means OK".
    std::string err = "stale";
    CHECK(ValidateRegexPattern(R"(^(?<level>\w+)\s+(?<message>.*)$)", err));
}

TEST_CASE("PatternMatchesLine returns false for empty pattern [regex]", "[regex_parser]")
{
    // The Validate button in the regex editor calls this on every
    // sample line; an empty pattern is a common intermediate
    // state during editing. It must fail closed rather than crash
    // or accept every input (which would show a misleading tick
    // in the UI).
    CHECK_FALSE(PatternMatchesLine("", "any line"));
}

TEST_CASE("PatternMatchesLine returns false on compile failure [regex]", "[regex_parser]")
{
    // Same intermediate state as above: mid-typed dangling group.
    // The one-off compile is discarded silently — callers wanting
    // the compile error text call `ValidateRegexPattern` first.
    CHECK_FALSE(PatternMatchesLine(R"((?<a)", "any line"));
}

TEST_CASE("PatternMatchesLine returns true only on a full anchored match [regex]", "[regex_parser]")
{
    // Anchored full-match semantics mirror the auto-detect probe
    // (`MatchesFullyForProbe`): "matches" means "a `RegexParser`
    // would emit a row for this line", never "the pattern appears
    // somewhere in the line". Regression guard for
    // `PCRE2_ANCHORED | PCRE2_ENDANCHORED` staying paired across
    // future refactors. A substring-only match must fail; a full
    // match must pass; a truly non-matching line must fail.
    constexpr std::string_view PATTERN = R"(^USER\s+(?<id>\d+)$)";
    CHECK(PatternMatchesLine(PATTERN, "USER 42"));
    CHECK_FALSE(PatternMatchesLine(PATTERN, "prefix USER 42 suffix"));
    CHECK_FALSE(PatternMatchesLine(PATTERN, "USER not-a-number"));
}

TEST_CASE("RegexParser::IsValid returns false for a missing file [regex]", "[regex_parser]")
{
    // `ProbeAutoDetectTemplates` short-circuits on
    // `!stream.is_open()`. Exercising that path keeps the coverage
    // sweep honest — a future refactor that dropped the check
    // would either crash on a fresh install (no session file yet)
    // or start returning true for paths that don't exist.
    const RegexParser parser{std::string{}};
    const std::filesystem::path missing = std::filesystem::temp_directory_path() / "regex_parser_missing_file_test.log";
    std::filesystem::remove(missing);
    REQUIRE_FALSE(std::filesystem::exists(missing));
    CHECK_FALSE(parser.IsValid(missing));
    CHECK_FALSE(DetectRegexTemplate(missing).has_value());
}

TEST_CASE("RegexParser leaves optional groups that did not match empty [regex]", "[regex_parser]")
{
    // Optional groups that didn't participate in the match land
    // as monostate. The syslog-style optional `[pid]` group is
    // the natural fixture: line 1 leaves `pid` absent, line 2
    // fills it.
    const RegexParser parser(R"(^(?<program>\w+)(?:\[(?<pid>\d+)\])?:\s+(?<message>.*)$)");
    const TestLogFile file("regex_tostring_monostate.log");
    file.Write(
        "systemd: System starting\n"
        "configd[17]: network changed\n"
    );

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 2);

    const auto &row0 = result.data.Lines()[0];
    CHECK(AsStringView(row0.GetValue("program")) == std::string_view{"systemd"});
    CHECK(AsStringView(row0.GetValue("message")) == std::string_view{"System starting"});
    CHECK(std::holds_alternative<std::monostate>(row0.GetValue("pid")));

    const auto &row1 = result.data.Lines()[1];
    CHECK(AsStringView(row1.GetValue("program")) == std::string_view{"configd"});
    CHECK(AsStringView(row1.GetValue("message")) == std::string_view{"network changed"});
    CHECK_FALSE(std::holds_alternative<std::monostate>(row1.GetValue("pid")));
}

TEST_CASE(
    "RegexParser: headerAnchor overrides the header probe (streaming)", "[regex_parser][multiline][header_anchor]"
)
{
    using namespace loglib;

    const std::string pattern = R"(^(?<ts>\d{4}-\d{2}-\d{2})\s+(?<level>\w+)\s+(?<message>.*)$)";
    const RegexTemplate anchored{
        .name = "test-header-anchor-streaming",
        .pattern = pattern,
        .sampleLines = {"2026-01-01 INFO ok"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::UntilNextHeader,
        .headerAnchor = R"(^\d{4}-)",
    };

    const RegexParser parser{anchored.pattern, anchored.continuationMode, anchored.headerAnchor};

    const std::string payload = "2026-01-01 ERROR boom\n"
                                "Traceback (most recent call last):\n"
                                "  File \"a.py\", line 7\n"
                                "During handling of the above exception, another exception occurred:\n"
                                "2026-01-02 INFO recovered\n";

    StreamLineSource source(
        std::filesystem::path("anchor_stream.log"), std::make_unique<InMemoryBytesProducer>(payload)
    );
    CollectingStreamSink sink;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);
    CHECK(FlattenSinkErrors(sink).empty());

    std::vector<LogLine *> lines;
    for (auto &b : sink.batches)
    {
        for (auto &l : b.lines)
        {
            lines.push_back(&l);
        }
    }
    REQUIRE(lines.size() == 2);

    const KeyId kMessage = sink.keys.Find("message");
    REQUIRE(kMessage != INVALID_KEY_ID);
    const LogValue v0 = lines[0]->GetValue(kMessage);
    const auto m0 = AsStringView(v0);
    REQUIRE(m0.has_value());
    CHECK(m0->contains("boom"));
    CHECK(m0->contains("Traceback"));
    CHECK(m0->contains("a.py"));
    CHECK(m0->contains("During handling"));

    const LogValue v1 = lines[1]->GetValue(kMessage);
    const auto m1 = AsStringView(v1);
    REQUIRE(m1.has_value());
    CHECK(*m1 == "recovered");
}

TEST_CASE(
    "RegexParser: headerAnchor overrides the header probe (static, cross-batch)",
    "[regex_parser][multiline][header_anchor]"
)
{
    using namespace loglib;

    const std::string pattern = R"(^(?<ts>\d{4}-\d{2}-\d{2})\s+(?<level>\w+)\s+(?<message>.*)$)";
    const RegexTemplate anchored{
        .name = "test-header-anchor-static",
        .pattern = pattern,
        .sampleLines = {"2026-01-01 INFO ok"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::UntilNextHeader,
        .headerAnchor = R"(^\d{4}-)",
    };

    ParserOptions options;
    options.batchSizeBytes = 24;

    const TestLogFile file("regex_header_anchor_crossbatch.log");
    file.Write(
        "2026-01-01 ERROR boom\n"
        "Traceback (most recent call last):\n"
        "  File \"a.py\", line 7\n"
        "During handling of the above exception, another exception occurred:\n"
        "  File \"b.py\", line 9\n"
        "2026-01-02 INFO recovered\n"
    );

    auto logFile = std::make_unique<LogFile>(file.GetFilePath());
    auto source = std::make_unique<FileLineSource>(std::move(logFile));
    FileLineSource *sourcePtr = source.get();
    internal::BufferingSink sink(std::move(source));

    RegexParser{anchored.pattern, anchored.continuationMode, anchored.headerAnchor}.ParseStreaming(
        *sourcePtr, sink, options
    );

    LogData data = sink.TakeData();
    const std::vector<loglib::ParseError> errors = sink.TakeErrors();
    for (const auto &e : errors)
    {
        UNSCOPED_INFO("parse error: " << loglib::FormatParseError(e));
    }
    REQUIRE(errors.empty());
    REQUIRE(data.Lines().size() == 2);

    const KeyId kMessage = data.Keys().Find("message");
    REQUIRE(kMessage != INVALID_KEY_ID);
    const LogValue v0 = data.Lines()[0].GetValue(kMessage);
    const auto m0 = AsStringView(v0);
    REQUIRE(m0.has_value());
    CHECK(m0->contains("boom"));
    CHECK(m0->contains("Traceback"));
    CHECK(m0->contains("a.py"));
    CHECK(m0->contains("During handling"));
    CHECK(m0->contains("b.py"));
}

TEST_CASE(
    "RegexParser: bad headerAnchor surfaces a compile error (streaming)", "[regex_parser][multiline][header_anchor]"
)
{
    using namespace loglib;

    const std::string pattern = R"(^(?<ts>\d{4}-\d{2}-\d{2})\s+(?<level>\w+)\s+(?<message>.*)$)";
    const RegexTemplate bad{
        .name = "test-header-anchor-bad-streaming",
        .pattern = pattern,
        .sampleLines = {"2026-01-01 INFO ok"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::UntilNextHeader,
        .headerAnchor = R"((?<a)",
    };

    const RegexParser parser{bad.pattern, bad.continuationMode, bad.headerAnchor};

    const std::string payload = "2026-01-01 ERROR boom\nTraceback\n2026-01-02 INFO ok\n";
    StreamLineSource source(
        std::filesystem::path("anchor_bad_stream.log"), std::make_unique<InMemoryBytesProducer>(payload)
    );
    CollectingStreamSink sink;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);

    std::vector<LogLine *> lines;
    for (auto &b : sink.batches)
    {
        for (auto &l : b.lines)
        {
            lines.push_back(&l);
        }
    }
    const auto errors = FlattenSinkErrors(sink);
    REQUIRE(errors.size() == 1);
    CHECK(loglib::FormatParseError(errors.front()).contains("Header anchor compile failed"));
    CHECK(lines.empty());
}

TEST_CASE("RegexParser: bad headerAnchor surfaces a compile error (static)", "[regex_parser][multiline][header_anchor]")
{
    using namespace loglib;

    const std::string pattern = R"(^(?<ts>\d{4}-\d{2}-\d{2})\s+(?<level>\w+)\s+(?<message>.*)$)";
    const RegexTemplate bad{
        .name = "test-header-anchor-bad-static",
        .pattern = pattern,
        .sampleLines = {"2026-01-01 INFO ok"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::UntilNextHeader,
        .headerAnchor = R"((?<a)",
    };

    const TestLogFile file("regex_header_anchor_bad_static.log");
    file.Write("2026-01-01 ERROR boom\nTraceback\n2026-01-02 INFO ok\n");

    auto logFile = std::make_unique<LogFile>(file.GetFilePath());
    auto source = std::make_unique<FileLineSource>(std::move(logFile));
    FileLineSource *sourcePtr = source.get();
    internal::BufferingSink sink(std::move(source));

    RegexParser{bad.pattern, bad.continuationMode, bad.headerAnchor}.ParseStreaming(*sourcePtr, sink, ParserOptions{});

    LogData data = sink.TakeData();
    const std::vector<loglib::ParseError> errors = sink.TakeErrors();
    REQUIRE(errors.size() == 1);
    CHECK(loglib::FormatParseError(errors.front()).contains("Header anchor compile failed"));
    CHECK(data.Lines().empty());
}

TEST_CASE(
    "RegexParser: headerAnchor ignored when continuationMode != UntilNextHeader",
    "[regex_parser][multiline][header_anchor]"
)
{
    using namespace loglib;

    const std::string pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    const std::string payload = "error boom\n"
                                "\tat com.example.Foo.bar(Foo.java:42)\n"
                                "\tat com.example.Baz.qux(Baz.java:7)\n"
                                "info recovered\n";

    auto runOnce = [&](const std::string &anchor) {
        const RegexTemplate extra{
            .name = "test-header-anchor-ignored",
            .pattern = pattern,
            .sampleLines = {"info ok"},
            .autoDetect = false,
            .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
            .description = "",
            .continuationMode = ContinuationMode::Indented,
            .headerAnchor = anchor,
        };

        const RegexParser parser{extra.pattern, extra.continuationMode, extra.headerAnchor};
        StreamLineSource source(
            std::filesystem::path("anchor_ignored.log"), std::make_unique<InMemoryBytesProducer>(payload)
        );
        CollectingStreamSink sink;
        parser.ParseStreaming(source, sink, ParserOptions{});
        REQUIRE(sink.finished);
        CHECK(FlattenSinkErrors(sink).empty());

        std::vector<std::string> joined;
        for (auto &b : sink.batches)
        {
            for (auto &l : b.lines)
            {
                const auto value = l.GetValue("message");
                const auto m = AsStringView(value);
                REQUIRE(m.has_value());
                joined.emplace_back(*m);
            }
        }
        return joined;
    };

    const auto control = runOnce("");
    const auto withAnchor = runOnce(R"(^\d{4}-)");
    CHECK(control == withAnchor);
}

TEST_CASE("ValidateHeaderAnchor accepts empty [regex]", "[regex_parser][header_anchor]")
{
    std::string err = "stale";
    CHECK(ValidateHeaderAnchor("", err));
    CHECK(err.empty());
}

TEST_CASE("ValidateHeaderAnchor rejects patterns that fail to compile [regex]", "[regex_parser][header_anchor]")
{
    std::string err;
    CHECK_FALSE(ValidateHeaderAnchor(R"((?<a)", err));
    CHECK(err.contains("Header anchor compile failed"));
}

TEST_CASE("ValidateHeaderAnchor accepts patterns without named groups [regex]", "[regex_parser][header_anchor]")
{
    std::string err = "stale";
    CHECK(ValidateHeaderAnchor(R"(^\d{4}-)", err));
    CHECK(err.empty());
}

TEST_CASE("RegexParser handles empty file cleanly [regex]", "[regex_parser]")
{
    // An empty regex file must be refused by `ParseFile(path)`
    // (its `file_size(file) == 0` guard fires before the parser
    // runs). Exercised here so the guard stays wired for the
    // regex path — a future refactor that let empty files reach
    // `RegexParser::ParseStreaming` would crash on the mmap
    // sizing rather than surface a clean error.
    const TestLogFile file("regex_empty.log");
    file.Write("");
    CHECK_THROWS_AS(ParseFile(file.GetFilePath()), std::runtime_error);
}
