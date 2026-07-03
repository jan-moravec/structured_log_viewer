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
    file.Write("Apr 28 04:02:03 host-a systemd: System starting\n"
               "Jun 27 01:47:20 host-b configd[17]: network changed\n");
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
    file.Write(R"({"level":"info","msg":"hello"})"
               "\n"
               R"({"level":"warn","msg":"world"})"
               "\n");
    CHECK_FALSE(parser.IsValid(file.GetFilePath()));
}

TEST_CASE("RegexParser default-constructed parse without pattern surfaces error [regex]", "[regex_parser]")
{
    // `LogFactory::Create(Regex)` returns a no-pattern instance.
    // Parsing must not crash; it surfaces one error and ends cleanly.
    const RegexParser parser;
    const TestLogFile file("regex_no_pattern.log");
    file.Write("Apr 28 04:02:03 host-a systemd: System starting\n"
               "Apr 28 04:02:04 host-a systemd: another line\n");

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
    file.Write("info 200 0.75 true\n"
               "warn 404 -1.5 false\n");

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
    file.Write("info:hello\n"
               "this line does not match\n"
               "error:boom\n");

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
    file.Write("systemd: System starting\n"
               "configd[17]: network changed\n");

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
    file.Write("Apr 28 04:02:03 host-a systemd: System starting\n"
               "Apr 28 04:02:04 host-b CRON[1234]: (root) CMD (test)\n");

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
    file.Write(R"({"level":"info","msg":"hello"})"
               "\n"
               R"({"level":"warn","msg":"world"})"
               "\n");

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
    file.Write("\xEF\xBB\xBF"
               "Apr 28 04:02:03 host-a systemd: System starting\n"
               "Apr 28 04:02:04 host-a systemd: another line\n");

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
    file.Write("systemd: System starting\n"
               "configd[17]: network changed\n");

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
