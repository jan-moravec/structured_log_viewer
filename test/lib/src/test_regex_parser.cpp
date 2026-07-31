#include "common.hpp"

#include <loglib/bytes_producer.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/internal/buffering_sink.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/parsers/regex_parser.hpp>
#include <loglib/regex_templates.hpp>
#include <loglib/stream_line_source.hpp>

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
    const RegexParser parser;
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
    const RegexParser parser;
    const TestLogFile file("regex_oneline.log");
    file.Write("Apr 28 04:02:03 host-a systemd: System starting\n");
    CHECK_FALSE(parser.IsValid(file.GetFilePath()));
}

TEST_CASE("RegexParser rejects JSON / logfmt files [regex]", "[regex_parser]")
{
    // Auto-detect precedence: a file another probe claims must not
    // also be claimed by Regex. JSON is the broadest non-regex shape.
    const RegexParser parser;
    const TestLogFile file("regex_rejects_json.log");
    file.Write(
        R"({"level":"info","msg":"hello"})"
        "\n"
        R"({"level":"warn","msg":"world"})"
        "\n"
    );
    CHECK_FALSE(parser.IsValid(file.GetFilePath()));
}

TEST_CASE("RegexParser default-constructed parse without pattern surfaces error [regex]", "[regex_parser]")
{
    // `LogFactory::Create(Regex)` returns a no-pattern instance.
    // Parsing must not crash; it surfaces one error and ends cleanly.
    const RegexParser parser;
    const TestLogFile file("regex_no_pattern.log");
    file.Write(
        "Apr 28 04:02:03 host-a systemd: System starting\n"
        "Apr 28 04:02:04 host-a systemd: another line\n"
    );

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors[0].contains("non-empty pattern"));
    CHECK(result.data.Lines().empty());
}

TEST_CASE("RegexParser unparsable pattern surfaces error [regex]", "[regex_parser]")
{
    const RegexParser parser(R"((?<a)"); // dangling group
    const TestLogFile file("regex_bad_pattern.log");
    file.Write("anything\nat all\n");

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.size() == 1);
    CHECK(result.errors[0].contains("Pattern compile failed"));
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
    CHECK(result.errors[0].contains("named capture groups"));
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
    CHECK(result.errors[0].contains("line 2"));
    CHECK(result.errors[0].contains("did not match"));
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

TEST_CASE("RegexParser ToString joins values in KeyId order [regex]", "[regex_parser]")
{
    // Best-effort round-trip: regex isn't invertible, so we accept
    // any space-separated form that includes every captured value.
    // Only used when the line's source bytes are gone.
    const RegexParser parser(R"(^(?<level>\w+)\s+(?<message>.*)$)");
    const TestLogFile file("regex_tostring.log");
    file.Write("info hello world\n");

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const std::string out = parser.ToString(result.data.Lines()[0]);
    CHECK(out.contains("info"));
    CHECK(out.contains("hello world"));
}

TEST_CASE("RegexParser pinned to empty pattern fails closed on the static path [regex]", "[regex_parser]")
{
    // Regression: `RegexParser("")` must NOT silently fall back
    // to `options.configuration->source->regexPattern`. The bug
    // was that the advanced overload treated an empty
    // `string_view` as "no explicit pattern" and read the config,
    // contradicting the streaming overload's fail-closed behaviour
    // for the same parser. The fix took the advanced overload to
    // `optional<string_view>` so "pinned to empty" stays distinct
    // from "no pattern pinned".
    const RegexParser parser{std::string{}};
    const TestLogFile file("regex_pinned_empty.log");
    file.Write("info hello\nwarn world\n");

    auto config = std::make_shared<LogConfiguration>();
    config->source = LogConfiguration::Source{
        .kind = LogConfiguration::Source::Kind::File,
        .format = LogConfiguration::Source::Format::Regex,
        .locators = {file.GetFilePath()},
        .locatorDedupKeys = {file.GetFilePath()},
        .regexPattern = R"(^(?<level>\w+)\s+(?<message>.*)$)",
    };

    auto logFile = std::make_unique<LogFile>(file.GetFilePath());
    auto source = std::make_unique<FileLineSource>(std::move(logFile));
    FileLineSource *sourcePtr = source.get();
    internal::BufferingSink sink(std::move(source));

    ParserOptions options;
    options.configuration = std::shared_ptr<const LogConfiguration>(config);
    parser.ParseStreaming(*sourcePtr, sink, options);

    LogData data = sink.TakeData();
    std::vector<std::string> errors = sink.TakeErrors();
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].contains("non-empty pattern"));
    CHECK(data.Lines().empty());
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
    // before `level` must produce a `ToString` reading
    // `<message> <level>` (the original order), not the
    // alphabetical permutation.
    const RegexParser parser(R"(^(?<message>[^|]*)\|(?<level>\w+)$)");
    const TestLogFile file("regex_tostring_order.log");
    file.Write("hello world|info\n");

    auto result = ParseFile(parser, file.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);

    const std::string out = parser.ToString(result.data.Lines()[0]);
    const auto messagePos = out.find("hello world");
    const auto levelPos = out.find("info");
    REQUIRE(messagePos != std::string::npos);
    REQUIRE(levelPos != std::string::npos);
    CHECK(messagePos < levelPos);
}

namespace
{

/// Single-shot in-memory `BytesProducer` for the live-tail regex
/// streaming test. Yields the pre-baked bytes once and reports
/// terminal EOF so the parser exits its drain loop without
/// parking on `WaitForBytes`. Mirrors `test_json_parser.cpp`.
class StreamingInMemoryProducer final : public loglib::BytesProducer
{
public:
    explicit StreamingInMemoryProducer(std::string bytes)
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

/// `LogParseSink` that records every emitted batch verbatim so
/// the test can assert on `newKeys`. Owns its `KeyIndex` so the
/// streaming parser interns into a sink-local index rather than
/// mutating a shared one.
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

} // namespace

// Regression for a live-tail / network-stream bug: the pattern's
// named capture groups are interned into `KeyIndex` before
// `RunStreamingParseLoop` starts, but the streaming overload used
// to drop the pre-intern baseline. `BatchCoalescer` then started
// its key cursor at the post-intern size, so every flushed batch
// reported an empty `newKeys` and `LogTable::AppendBatch` never
// created the columns -- streaming regex sessions ingested rows
// with no visible columns. Fails on the pre-fix codebase; passes
// once `newKeyBaseline` is threaded through.
TEST_CASE(
    "RegexParser streaming surfaces named-group columns via newKeys [regex][stream_line_source]", "[regex_parser]"
)
{
    using namespace loglib;

    const RegexParser parser(R"(^(?<level>\w+)\s+(?<message>.*)$)");

    const std::string payload = "info hello\n"
                                "warn world\n"
                                "error boom\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<StreamingInMemoryProducer>(payload));

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
std::vector<std::string> FlattenSinkErrors(const CollectingStreamSink &sink)
{
    std::vector<std::string> all;
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
std::vector<std::string> RunStreamingParseCollectingErrors(std::string_view pattern, std::string_view payload)
{
    const RegexParser parser{std::string{pattern}};
    StreamLineSource source(
        std::filesystem::path("regex_stream_error.log"),
        std::make_unique<StreamingInMemoryProducer>(std::string{payload})
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
    // line. Prior to the newKeyBaseline fix the streaming overload
    // dropped this error entirely because `EmitErrorAndFinish`
    // wasn't wired for the streaming flush thresholds.
    const RegexParser parser;
    StreamLineSource source(
        std::filesystem::path("regex_stream_empty_pattern.log"),
        std::make_unique<StreamingInMemoryProducer>("one\ntwo\n")
    );
    CollectingStreamSink sink;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);
    CHECK_FALSE(sink.finishedCancelled);
    const auto errors = FlattenSinkErrors(sink);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].contains("non-empty pattern"));
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
    CHECK(errors[0].contains("Pattern compile failed"));
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
    CHECK(errors[0].contains("named capture groups"));
}

// -----------------------------------------------------------------------------
// Multi-line record tests (streaming pipeline)
//
// `RegexTemplate::continuationMode` on a shipped or user-registered
// template drives multi-line handling for the streaming path.
// - `Indented`: O(1) first-byte whitespace check. Java Logback
//   (built-in `java_log.json`) ships with this mode.
// - `UntilNextHeader`: PCRE2 anchored+partial-hard header check.
//   Covers Python-style traces whose continuation lines mix
//   indented and un-indented text.
// Static (file-based) pipeline is a follow-up slice.
// -----------------------------------------------------------------------------

namespace
{

/// Register @p extras for the duration of a test case, resetting on
/// scope exit. Templates registered via `SetExtraRegexTemplates` are
/// process-global, so failure to reset would leak across tests.
class ScopedExtraTemplates
{
public:
    explicit ScopedExtraTemplates(std::span<const loglib::RegexTemplate> extras)
    {
        loglib::SetExtraRegexTemplates(extras);
    }
    ~ScopedExtraTemplates()
    {
        loglib::SetExtraRegexTemplates({});
    }
    ScopedExtraTemplates(const ScopedExtraTemplates &) = delete;
    ScopedExtraTemplates &operator=(const ScopedExtraTemplates &) = delete;
    ScopedExtraTemplates(ScopedExtraTemplates &&) = delete;
    ScopedExtraTemplates &operator=(ScopedExtraTemplates &&) = delete;
};

} // namespace

TEST_CASE(
    "RegexParser streaming: indented continuation folds into the last named group",
    "[regex_parser][stream_line_source][multiline]"
)
{
    using namespace loglib;

    // A minimal Java-shape pattern with `level` + `message`. Ship
    // it as an extra template with `continuationMode: Indented` so
    // `FindTemplateByPattern` in `ParseStreaming` resolves the
    // mode. `\t`-indented lines are continuations of the prior
    // `message`.
    const std::string pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    RegexTemplate extra{
        .name = "test-indented-multiline",
        .pattern = pattern,
        .sampleLines = {"info hello"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::Indented,
        .headerAnchor = "",
    };
    const RegexTemplate extras[] = {extra};
    const ScopedExtraTemplates registration(extras);

    const RegexParser parser{std::string{pattern}};

    // Header + two indented continuation lines + another header.
    const std::string payload = "error boom\n"
                                "\tat com.example.Foo.bar(Foo.java:42)\n"
                                "\tat com.example.Baz.qux(Baz.java:7)\n"
                                "info recovered\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<StreamingInMemoryProducer>(payload));
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
        *m0
        == "boom\n"
           "\tat com.example.Foo.bar(Foo.java:42)\n"
           "\tat com.example.Baz.qux(Baz.java:7)"
    );
    CHECK(*m1 == "recovered");

    // RawLine on the multi-line record returns the joined bytes so
    // Copy / Detail-pane consumers naturally see the whole trace.
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

    // Streaming used to silently drop blank lines that appeared
    // inside a multi-line record: the pending record's `rawText`
    // would go straight from header to continuation, so `RawLine`
    // reported "header\n<cont>" while the on-disk / static path
    // returned "header\n\n<cont>" for identical bytes. This fixture
    // pins the field value (blanks not folded, matches static) and
    // the raw joined bytes (blanks folded, also matches static).
    // Trailing blanks after the last continuation must NOT leak
    // into the record either.
    const std::string pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    RegexTemplate extra{
        .name = "test-stream-multiline-blank",
        .pattern = pattern,
        .sampleLines = {"info hello"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::Indented,
        .headerAnchor = "",
    };
    const RegexTemplate extras[] = {extra};
    const ScopedExtraTemplates registration(extras);

    const RegexParser parser{std::string{pattern}};

    // Record 0: header, blank, cont, blank (trailing separator).
    // Record 1: single-line.
    const std::string payload =
        "error boom\n"
        "\n"
        "\tat com.example.Foo.bar(Foo.java:42)\n"
        "\n"
        "info recovered\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<StreamingInMemoryProducer>(payload));
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
    // Field value skips the blank -- one '\n' separator between
    // header content and continuation, matching the static path's
    // in-batch splice.
    CHECK(*m0 == "boom\n\tat com.example.Foo.bar(Foo.java:42)");

    // RawLine spans the blank as a bare '\n' (matches mmap-based
    // `LogFile::GetLine` for the same input).
    const std::string raw0 = source.RawLine(lines[0]->LineId());
    CHECK(raw0.contains("boom\n\n\tat"));
    CHECK_FALSE(raw0.contains("info recovered"));
    // Trailing blank between the record and the next header is a
    // separator, not part of record 0's rawText.
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

    // Pattern requires a leading `[LEVEL]` header. Continuation
    // lines in this format mix indented and un-indented text (a
    // Python-style "During handling of the above exception"). The
    // PCRE2 anchored-partial-hard probe classifies them as
    // continuations because they don't start with `[`.
    const std::string pattern = R"(^\[(?<level>\w+)\]\s+(?<message>.*)$)";
    RegexTemplate extra{
        .name = "test-header-multiline",
        .pattern = pattern,
        .sampleLines = {"[ERROR] boom"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::UntilNextHeader,
        .headerAnchor = "",
    };
    const RegexTemplate extras[] = {extra};
    const ScopedExtraTemplates registration(extras);

    const RegexParser parser{std::string{pattern}};

    const std::string payload = "[ERROR] boom\n"
                                "Traceback (most recent call last):\n"
                                "During handling of the above exception, another exception occurred:\n"
                                "  File \"a.py\", line 7\n"
                                "[INFO] recovered\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<StreamingInMemoryProducer>(payload));
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
    // All three non-header lines (both indented and un-indented) fold in.
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

    // Same pattern as the indented test but registered with
    // `ContinuationMode::None` (the default). Indented lines are
    // now parse errors, matching the pre-feature contract exactly.
    const std::string pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    RegexTemplate extra{
        .name = "test-legacy-singleline",
        .pattern = pattern,
        .sampleLines = {"info hello"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::None,
        .headerAnchor = "",
    };
    const RegexTemplate extras[] = {extra};
    const ScopedExtraTemplates registration(extras);

    const RegexParser parser{std::string{pattern}};

    const std::string payload = "error boom\n"
                                "\tat com.example.Foo.bar(Foo.java:42)\n"
                                "info recovered\n";

    StreamLineSource source(std::filesystem::path("memory.log"), std::make_unique<StreamingInMemoryProducer>(payload));
    CollectingStreamSink sink;
    parser.ParseStreaming(source, sink, ParserOptions{});

    REQUIRE(sink.finished);

    // `\w+` doesn't match a leading tab, so the continuation line
    // fails to match under ContinuationMode::None and surfaces as
    // a per-line parse error — matching the pre-feature behaviour
    // verbatim. Two rows emit (the un-indented headers) and one
    // error is captured.
    std::vector<LogLine *> lines;
    std::vector<std::string> errors;
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
    RegexTemplate extra{
        .name = "test-static-indented",
        .pattern = pattern,
        .sampleLines = {"info hello"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::Indented,
        .headerAnchor = "",
    };
    const RegexTemplate extras[] = {extra};
    const ScopedExtraTemplates registration(extras);

    const RegexParser parser{std::string{pattern}};

    // Two records; the first spans three physical lines.
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
        UNSCOPED_INFO("parse error: " << e);
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
        *m0
        == "boom\n"
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
    RegexTemplate extra{
        .name = "test-static-crossbatch",
        .pattern = pattern,
        .sampleLines = {"info hello"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::Indented,
        .headerAnchor = "",
    };
    const RegexTemplate extras[] = {extra};
    const ScopedExtraTemplates registration(extras);

    // Force a tiny batch size so continuation lines almost certainly
    // straddle a batch boundary. Stage C's hold-back + splice path is
    // the mechanism under test.
    internal::AdvancedParserOptions advanced;
    advanced.batchSizeBytes = 32; // deliberately smaller than one record

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

    RegexParser parser{pattern};
    parser.ParseStreaming(*sourcePtr, sink, ParserOptions{}, advanced, std::optional<std::string_view>{pattern});

    auto data = sink.TakeData();
    const auto errors = sink.TakeErrors();
    for (const auto &e : errors)
    {
        UNSCOPED_INFO("parse error: " << e);
    }
    REQUIRE(errors.empty());
    REQUIRE(data.Lines().size() == 2);

    const KeyId kMessage = data.Keys().Find("message");
    REQUIRE(kMessage != INVALID_KEY_ID);

    const LogValue v0 = data.Lines()[0].GetValue(kMessage);
    const auto m0 = AsStringView(v0);
    REQUIRE(m0.has_value());
    // The joined message must include every continuation line
    // regardless of which side of the batch boundary it landed on.
    CHECK(m0->contains("boom"));
    CHECK(m0->contains("Foo.java:42"));
    CHECK(m0->contains("Baz.java:7"));
    CHECK(m0->contains("Wibble.java:99"));

    // Stage C used to overwrite the held record's trailing offset
    // with the LAST leading-continuation offset on every cross-batch
    // splice, silently dropping the intermediate physical-line
    // boundaries from `mLineOffsets`. That collapsed `GetLineCount`,
    // shifted every per-physical-line lookup, and made the widening
    // guard in `RegisterMultiLineRecord` skip the header (its
    // `lastLineIdx` ran past the shrunken offsets array). Assert
    // the per-physical-line indexing survives the splice.
    LogFile &parsedFile = sourcePtr->File();
    REQUIRE(parsedFile.GetLineCount() == 5);
    CHECK(parsedFile.GetLine(1) == "\tat com.example.Foo.bar(Foo.java:42)");
    CHECK(parsedFile.GetLine(2) == "\tat com.example.Baz.qux(Baz.java:7)");
    CHECK(parsedFile.GetLine(3) == "\tat com.example.Wibble.wobble(Wibble.java:99)");
    CHECK(parsedFile.GetLine(4) == "info recovered");

    // Header widening still covers the whole trace and stops at the
    // last continuation (no bleed into the next record's line).
    const std::string joined = sourcePtr->RawLine(data.Lines()[0].LineId());
    CHECK(joined.contains("error boom"));
    CHECK(joined.contains("Foo.java:42"));
    CHECK(joined.contains("Wibble.java:99"));
    CHECK_FALSE(joined.contains("info recovered"));

    // Second record's `LineId` used to overshoot `mLineOffsets` and
    // trigger `std::out_of_range` on the copy / detail-dock path.
    const std::string secondRaw = sourcePtr->RawLine(data.Lines()[1].LineId());
    CHECK(secondRaw == "info recovered");
}

TEST_CASE(
    "RegexParser file: blank line inside cross-batch leading continuations stays in the record's span",
    "[regex_parser][file_line_source][multiline][cross_batch]"
)
{
    using namespace loglib;

    // Cross-batch leading continuations used to count only actual
    // continuation lines, ignoring blank lines interspersed with them.
    // Stage C would then move too few offsets onto the held record
    // and set its `lastLineIdx` short of the real last content line,
    // making `LogFile::GetLine(headerLineId)` truncate the joined
    // record right before the continuation. This fixture pins a
    // blank line squarely inside the leading region so a regression
    // to that arithmetic flips the assertion below.
    const std::string pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    RegexTemplate extra{
        .name = "test-crossbatch-blank-leading",
        .pattern = pattern,
        .sampleLines = {"info hello"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::Indented,
        .headerAnchor = "",
    };
    const RegexTemplate extras[] = {extra};
    const ScopedExtraTemplates registration(extras);

    // Tiny batches so the header lands in batch 0 and the blank +
    // continuation + next header land in batch 1 as leading
    // continuations.
    internal::AdvancedParserOptions advanced;
    advanced.batchSizeBytes = 16;

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

    RegexParser parser{pattern};
    parser.ParseStreaming(*sourcePtr, sink, ParserOptions{}, advanced, std::optional<std::string_view>{pattern});

    auto data = sink.TakeData();
    const auto errors = sink.TakeErrors();
    for (const auto &e : errors)
    {
        UNSCOPED_INFO("parse error: " << e);
    }
    REQUIRE(errors.empty());
    REQUIRE(data.Lines().size() == 2);

    // Per-physical-line indexing must still resolve every line to
    // its bytes -- the blank shows up as an empty string, and the
    // continuation lives at its own physical index.
    LogFile &parsedFile = sourcePtr->File();
    REQUIRE(parsedFile.GetLineCount() == 4);
    CHECK(parsedFile.GetLine(1) == "");
    CHECK(parsedFile.GetLine(2) == "\tat com.example.Foo.bar(Foo.java:42)");
    CHECK(parsedFile.GetLine(3) == "info recovered");

    // The multi-line record's joined text must include the blank
    // line's '\n' AND the trailing continuation.
    const std::string joined = sourcePtr->RawLine(data.Lines()[0].LineId());
    CHECK(joined.contains("error boom"));
    CHECK(joined.contains("Foo.java:42"));
    // "boom" + "\n" (line 0's terminator) + "" (blank) + "\n"
    // (blank's terminator) is what the joined bytes should carry.
    CHECK(joined.contains("boom\n\n\tat"));
    CHECK_FALSE(joined.contains("info recovered"));

    // Message field also sees the continuation. The blank line
    // itself doesn't contribute any bytes to the field (matches the
    // in-batch semantics), only its physical-line index counts.
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

    // Second regression axis for the pending-blank promotion logic:
    // if the leading region contains blanks with NO continuation
    // after them, they must NOT extend the held tail (blanks between
    // records are separators, exactly like the in-batch case).
    const std::string pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    RegexTemplate extra{
        .name = "test-crossbatch-blank-no-cont",
        .pattern = pattern,
        .sampleLines = {"info hello"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::Indented,
        .headerAnchor = "",
    };
    const RegexTemplate extras[] = {extra};
    const ScopedExtraTemplates registration(extras);

    internal::AdvancedParserOptions advanced;
    advanced.batchSizeBytes = 16;

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

    RegexParser parser{pattern};
    parser.ParseStreaming(*sourcePtr, sink, ParserOptions{}, advanced, std::optional<std::string_view>{pattern});

    auto data = sink.TakeData();
    const auto errors = sink.TakeErrors();
    REQUIRE(errors.empty());
    REQUIRE(data.Lines().size() == 2);

    // `error boom` is a single-line record; the blank is a between-
    // records separator that stays a normal physical line.
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
    const RegexParser parser;
    const std::filesystem::path missing = std::filesystem::temp_directory_path() / "regex_parser_missing_file_test.log";
    std::filesystem::remove(missing);
    REQUIRE_FALSE(std::filesystem::exists(missing));
    CHECK_FALSE(parser.IsValid(missing));
    CHECK_FALSE(DetectRegexTemplate(missing).has_value());
}

TEST_CASE("RegexParser ToString skips monostate columns [regex]", "[regex_parser]")
{
    // Optional groups that didn't participate in the match land
    // as monostate and must not surface in `ToString`'s space-
    // joined output. The syslog-style optional `[pid]` group is
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

    const std::string row0 = parser.ToString(result.data.Lines()[0]);
    CHECK(row0.contains("systemd"));
    CHECK(row0.contains("System starting"));
    // No `[pid]`-shaped digits in the output; the only digits in
    // the sample come from an optional group that didn't fire.
    CHECK(row0.find_first_of("0123456789") == std::string::npos);

    const std::string row1 = parser.ToString(result.data.Lines()[1]);
    CHECK(row1.contains("configd"));
    CHECK(row1.contains("17"));
    CHECK(row1.contains("network changed"));
}

TEST_CASE("RegexParser static overload with explicit pattern overrides configuration [regex]", "[regex_parser]")
{
    // The advanced-tuning overload (`RegexParser::ParseStreaming`
    // static) takes an `optional<string_view>` explicit pattern.
    // A present value must override any `regexPattern` on the
    // configuration snapshot — this is what a benchmark driver
    // needs to bypass the pinned-pattern parser without touching
    // the caller's config. Also asserts a non-empty override
    // succeeds where the configuration would have failed
    // (mismatched pattern), so a bug that quietly consulted the
    // configuration would flip the test to red.
    const std::string_view explicitPattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    const TestLogFile file("regex_static_advanced.log");
    file.Write("info hello\nwarn world\n");

    auto config = std::make_shared<LogConfiguration>();
    config->source = LogConfiguration::Source{
        .kind = LogConfiguration::Source::Kind::File,
        .format = LogConfiguration::Source::Format::Regex,
        .locators = {file.GetFilePath()},
        .locatorDedupKeys = {file.GetFilePath()},
        // Deliberately wrong: if the overload ever falls back to
        // this pattern the columns would come out as (`k`, `v`)
        // and neither of the fixture lines would match at all.
        .regexPattern = R"(^(?<k>\S+)=(?<v>\S+)$)",
    };
    auto logFile = std::make_unique<LogFile>(file.GetFilePath());
    auto source = std::make_unique<FileLineSource>(std::move(logFile));
    FileLineSource *sourcePtr = source.get();
    internal::BufferingSink sink(std::move(source));

    ParserOptions options;
    options.configuration = std::shared_ptr<const LogConfiguration>(config);
    RegexParser::ParseStreaming(*sourcePtr, sink, options, internal::AdvancedParserOptions{}, explicitPattern);

    LogData data = sink.TakeData();
    const std::vector<std::string> errors = sink.TakeErrors();
    CHECK(errors.empty());
    REQUIRE(data.Lines().size() == 2);
    CHECK(AsStringView(data.Lines()[0].GetValue("level")) == std::string_view{"info"});
    CHECK(AsStringView(data.Lines()[1].GetValue("message")) == std::string_view{"world"});
}

TEST_CASE(
    "RegexParser: headerAnchor overrides the header probe (streaming)",
    "[regex_parser][multiline][header_anchor]"
)
{
    using namespace loglib;

    // Main pattern requires the level word plus a message; header
    // anchor is a cheaper `^\d{4}-` prefix probe. Both refuse the
    // continuation lines, but this test asserts the wiring by
    // registering a `headerAnchor` and observing identical folding.
    const std::string pattern = R"(^(?<ts>\d{4}-\d{2}-\d{2})\s+(?<level>\w+)\s+(?<message>.*)$)";
    RegexTemplate anchored{
        .name = "test-header-anchor-streaming",
        .pattern = pattern,
        .sampleLines = {"2026-01-01 INFO ok"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::UntilNextHeader,
        .headerAnchor = R"(^\d{4}-)",
    };
    const RegexTemplate extras[] = {anchored};
    const ScopedExtraTemplates registration(extras);

    const RegexParser parser{std::string{pattern}};

    const std::string payload = "2026-01-01 ERROR boom\n"
                                "Traceback (most recent call last):\n"
                                "  File \"a.py\", line 7\n"
                                "During handling of the above exception, another exception occurred:\n"
                                "2026-01-02 INFO recovered\n";

    StreamLineSource source(std::filesystem::path("anchor_stream.log"), std::make_unique<StreamingInMemoryProducer>(payload));
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
    // The joined message must contain every continuation line —
    // the anchor probe rejected them as non-headers just like the
    // main pattern would have.
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
    RegexTemplate anchored{
        .name = "test-header-anchor-static",
        .pattern = pattern,
        .sampleLines = {"2026-01-01 INFO ok"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::UntilNextHeader,
        .headerAnchor = R"(^\d{4}-)",
    };
    const RegexTemplate extras[] = {anchored};
    const ScopedExtraTemplates registration(extras);

    // Force a tiny batch so the continuation run almost certainly
    // straddles a Stage B boundary. Regression guard for Stage C's
    // hold-back path re-using the header anchor across workers.
    internal::AdvancedParserOptions advanced;
    advanced.batchSizeBytes = 24;

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

    RegexParser::ParseStreaming(
        *sourcePtr, sink, ParserOptions{}, advanced, std::optional<std::string_view>{pattern}
    );

    LogData data = sink.TakeData();
    const std::vector<std::string> errors = sink.TakeErrors();
    for (const auto &e : errors)
    {
        UNSCOPED_INFO("parse error: " << e);
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
    "RegexParser: bad headerAnchor surfaces a compile error (streaming)",
    "[regex_parser][multiline][header_anchor]"
)
{
    using namespace loglib;

    const std::string pattern = R"(^(?<ts>\d{4}-\d{2}-\d{2})\s+(?<level>\w+)\s+(?<message>.*)$)";
    RegexTemplate bad{
        .name = "test-header-anchor-bad-streaming",
        .pattern = pattern,
        .sampleLines = {"2026-01-01 INFO ok"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::UntilNextHeader,
        .headerAnchor = R"((?<a)", // dangling group
    };
    const RegexTemplate extras[] = {bad};
    const ScopedExtraTemplates registration(extras);

    const RegexParser parser{std::string{pattern}};

    const std::string payload = "2026-01-01 ERROR boom\nTraceback\n2026-01-02 INFO ok\n";
    StreamLineSource source(std::filesystem::path("anchor_bad_stream.log"), std::make_unique<StreamingInMemoryProducer>(payload));
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
    // Bad anchor must fail closed: exactly one error, zero rows.
    const auto errors = FlattenSinkErrors(sink);
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().contains("Header anchor compile failed"));
    CHECK(lines.empty());
}

TEST_CASE(
    "RegexParser: bad headerAnchor surfaces a compile error (static)",
    "[regex_parser][multiline][header_anchor]"
)
{
    using namespace loglib;

    const std::string pattern = R"(^(?<ts>\d{4}-\d{2}-\d{2})\s+(?<level>\w+)\s+(?<message>.*)$)";
    RegexTemplate bad{
        .name = "test-header-anchor-bad-static",
        .pattern = pattern,
        .sampleLines = {"2026-01-01 INFO ok"},
        .autoDetect = false,
        .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
        .description = "",
        .continuationMode = ContinuationMode::UntilNextHeader,
        .headerAnchor = R"((?<a)",
    };
    const RegexTemplate extras[] = {bad};
    const ScopedExtraTemplates registration(extras);

    const TestLogFile file("regex_header_anchor_bad_static.log");
    file.Write("2026-01-01 ERROR boom\nTraceback\n2026-01-02 INFO ok\n");

    auto logFile = std::make_unique<LogFile>(file.GetFilePath());
    auto source = std::make_unique<FileLineSource>(std::move(logFile));
    FileLineSource *sourcePtr = source.get();
    internal::BufferingSink sink(std::move(source));

    RegexParser::ParseStreaming(
        *sourcePtr,
        sink,
        ParserOptions{},
        internal::AdvancedParserOptions{},
        std::optional<std::string_view>{pattern}
    );

    LogData data = sink.TakeData();
    const std::vector<std::string> errors = sink.TakeErrors();
    REQUIRE(errors.size() == 1);
    CHECK(errors.front().contains("Header anchor compile failed"));
    CHECK(data.Lines().empty());
}

TEST_CASE(
    "RegexParser: headerAnchor ignored when continuationMode != UntilNextHeader",
    "[regex_parser][multiline][header_anchor]"
)
{
    using namespace loglib;

    // Indented mode uses a byte-cheap "starts with space/tab"
    // check, never the header probe. Setting `headerAnchor` in
    // this mode must be silently ignored so the field's
    // "ignored outside UntilNextHeader" contract is enforced
    // at the parser level, not just the editor.
    const std::string pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    const std::string payload =
        "error boom\n"
        "\tat com.example.Foo.bar(Foo.java:42)\n"
        "\tat com.example.Baz.qux(Baz.java:7)\n"
        "info recovered\n";

    auto runOnce = [&](const std::string &anchor) {
        RegexTemplate extra{
            .name = "test-header-anchor-ignored",
            .pattern = pattern,
            .sampleLines = {"info ok"},
            .autoDetect = false,
            .priority = USER_TEMPLATE_DEFAULT_PRIORITY,
            .description = "",
            .continuationMode = ContinuationMode::Indented,
            .headerAnchor = anchor,
        };
        const RegexTemplate extras[] = {extra};
        const ScopedExtraTemplates registration(extras);

        const RegexParser parser{std::string{pattern}};
        StreamLineSource source(
            std::filesystem::path("anchor_ignored.log"), std::make_unique<StreamingInMemoryProducer>(payload)
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
                const auto m = AsStringView(l.GetValue("message"));
                REQUIRE(m.has_value());
                joined.emplace_back(*m);
            }
        }
        return joined;
    };

    const auto control = runOnce("");
    const auto withAnchor = runOnce(R"(^\d{4}-)");
    // A deliberately mismatched `headerAnchor` (matches nothing in
    // this fixture) must not perturb Indented-mode folding one bit.
    CHECK(control == withAnchor);
}

TEST_CASE("ValidateHeaderAnchor accepts empty [regex]", "[regex_parser][header_anchor]")
{
    // Empty anchor is the shipped "reuse the main pattern" path;
    // callers key their UI off "empty error means OK", so
    // `errorOut` must come back clear even if it was pre-populated
    // with stale text.
    std::string err = "stale";
    CHECK(ValidateHeaderAnchor("", err));
    CHECK(err.empty());
}

TEST_CASE(
    "ValidateHeaderAnchor rejects patterns that fail to compile [regex]",
    "[regex_parser][header_anchor]"
)
{
    // Wording of the surfaced error mirrors the runtime error the
    // streaming pipeline emits on a bad anchor so both surfaces
    // (editor pre-flight and parse-time error stream) read
    // identically.
    std::string err;
    CHECK_FALSE(ValidateHeaderAnchor(R"((?<a)", err));
    CHECK(err.contains("Header anchor compile failed"));
}

TEST_CASE(
    "ValidateHeaderAnchor accepts patterns without named groups [regex]",
    "[regex_parser][header_anchor]"
)
{
    // The anchor is a boolean probe, not a schema; a plain
    // `^\d{4}-` (no `(?<Name>...)`) is a perfectly valid
    // header anchor and must be accepted.
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
