#include "common.hpp"

#include <loglib/core/bytes_producer.hpp>
#include <loglib/core/file_line_source.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_data.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/core/log_value.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/parse/auto_detect_parser.hpp>
#include <loglib/parse/format_detection.hpp>
#include <loglib/parse/internal/buffering_sink.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/parse/parsers/logfmt_parser.hpp>
#include <loglib/parse/parsers/regex_parser.hpp>
#include <loglib/parse/regex_templates.hpp>
#include <loglib/schema/log_configuration.hpp>
#include <loglib/table/log_table.hpp>
#include <loglib/time/time_zone_context.hpp>
#include <loglib/time/timestamp_parsing.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace loglib;

namespace
{

constexpr std::int64_t HOUR_US = 3'600'000'000;

/** @brief Microseconds since the epoch of a wall-clock time read as UTC. */
std::int64_t Micros(int year, unsigned month, unsigned day, int hour, int minute, int second)
{
    const auto days =
        std::chrono::sys_days{std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}};
    const auto total = std::chrono::duration_cast<std::chrono::microseconds>(days.time_since_epoch()) +
                       std::chrono::hours{hour} + std::chrono::minutes{minute} + std::chrono::seconds{second};
    return total.count();
}

/** @brief Serves a string once and then reports the end of the stream. */
class StringProducer final : public BytesProducer
{
public:
    explicit StringProducer(std::string bytes)
        : mBytes(std::move(bytes))
    {
    }

    size_t Read(std::span<char> buffer) override
    {
        const size_t n = std::min(mBytes.size() - mCursor, buffer.size());
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
        return "string";
    }

private:
    std::string mBytes;
    size_t mCursor = 0;
    bool mClosed = false;
};

struct CollectingSink final : LogParseSink
{
    KeyIndex keys;
    std::vector<RowBatch> batches;
    bool finished = false;

    KeyIndex &Keys() override
    {
        return keys;
    }
    void OnStarted() override
    {
    }
    void OnBatch(RowBatch batch) override
    {
        batches.push_back(std::move(batch));
    }
    void OnFinished(bool /*cancelled*/) override
    {
        finished = true;
    }

    [[nodiscard]] std::vector<const LogLine *> Lines() const
    {
        std::vector<const LogLine *> lines;
        for (const RowBatch &batch : batches)
        {
            for (const LogLine &line : batch.lines)
            {
                lines.push_back(&line);
            }
        }
        return lines;
    }

    [[nodiscard]] std::vector<std::string> Errors() const
    {
        std::vector<std::string> errors;
        for (const RowBatch &batch : batches)
        {
            errors.insert(errors.end(), batch.errors.begin(), batch.errors.end());
        }
        return errors;
    }
};

Column TimeColumn(std::string key, std::string parseFormat)
{
    Column column;
    column.header = key;
    column.keys = {std::move(key)};
    column.type = ColumnType::Time;
    column.printFormat = "%F %H:%M:%S";
    column.parseFormats = {std::move(parseFormat)};
    return column;
}

/** @brief Parses a JSON Lines file and returns the TimeStamp in the first row's "ts" field, in microseconds. */
std::int64_t ParsedTimestampMicros(const std::string &content, const ParserOptions &options)
{
    const TestLogFile file("explicit_time_input.json");
    file.Write(content);
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(file.GetFilePath()));
    FileLineSource *sourcePtr = source.get();
    internal::BufferingSink sink(std::move(source));
    JsonParser{}.ParseStreaming(*sourcePtr, sink, options);
    const LogData data = sink.TakeData();
    REQUIRE(data.Lines().size() >= 1);
    const LogValue value = data.Lines()[0].GetValue("ts");
    const auto *stamp = std::get_if<TimeStamp>(&value);
    REQUIRE(stamp != nullptr);
    return stamp->time_since_epoch().count();
}

/** @brief Sets the process-wide zone for one scope. Only these tests use it, to prove the library ignores it. */
class ScopedProcessZone
{
public:
    explicit ScopedProcessZone(const std::string &zone)
        : mPrevious(ProcessDefaultTimeZone())
    {
        SetProcessDefaultTimeZone(TimeZoneContext::Load(FindTestTzdata(), zone));
    }
    ScopedProcessZone(const ScopedProcessZone &) = delete;
    ScopedProcessZone &operator=(const ScopedProcessZone &) = delete;
    ~ScopedProcessZone()
    {
        SetProcessDefaultTimeZone(mPrevious);
    }

private:
    TimeZoneContext mPrevious;
};

} // namespace

TEST_CASE("TimeColumnsOf keeps only Time columns, in order", "[explicit_time_inputs]")
{
    LogConfiguration configuration;
    Column text;
    text.header = "message";
    text.keys = {"message"};
    configuration.columns.push_back(text);
    configuration.columns.push_back(TimeColumn("first", "%F %T"));
    Column number;
    number.header = "latency";
    number.keys = {"latency"};
    number.type = ColumnType::Any;
    configuration.columns.push_back(number);
    configuration.columns.push_back(TimeColumn("second", "%FT%T"));

    const std::vector<Column> columns = TimeColumnsOf(configuration);
    REQUIRE(columns.size() == 2);
    CHECK(columns[0].header == "first");
    CHECK(columns[1].header == "second");
    CHECK(columns[1].parseFormats == std::vector<std::string>{"%FT%T"});
    CHECK(TimeColumnsOf(LogConfiguration{}).empty());
}

TEST_CASE("Two tables in one process format the same timestamp in their own display zones", "[explicit_time_inputs]")
{
    const ScopedProcessZone processZone("Asia/Tokyo");
    const TimeZoneContext berlin = TimeZoneContext::Load(FindTestTzdata(), "Europe/Berlin");
    const TimeZoneContext newYork = TimeZoneContext::Load(FindTestTzdata(), "America/New_York");

    // The files stay alive as long as the tables that map them.
    std::vector<std::unique_ptr<TestLogFile>> files;
    const auto makeTable = [&files] {
        files.push_back(
            std::make_unique<TestLogFile>("explicit_time_display_" + std::to_string(files.size()) + ".json")
        );
        const TestLogFile &file = *files.back();
        file.Write("{}\n");
        auto source = file.CreateFileLineSource();
        FileLineSource *sourcePtr = source.get();
        KeyIndex keys;
        std::vector<LogLine> lines;
        lines.emplace_back(
            LogMap{{"ts", TimeStamp{std::chrono::microseconds{Micros(2026, 1, 15, 12, 0, 0)}}}}, keys, *sourcePtr, 0
        );
        LogData data(std::move(source), std::move(lines), std::move(keys));
        data.MarkTimestampsParsed();
        LogConfigurationManager manager;
        LogConfiguration configuration;
        configuration.columns.push_back(TimeColumn("ts", "%F %T"));
        manager.SetConfiguration(std::move(configuration));
        return LogTable(std::move(data), std::move(manager));
    };

    LogTable utcTable = makeTable();
    LogTable berlinTable = makeTable();
    LogTable newYorkTable = makeTable();
    berlinTable.SetDisplayTimeZone(berlin);
    newYorkTable.SetDisplayTimeZone(newYork);

    CHECK(utcTable.GetFormattedValue(0, 0) == "2026-01-15 12:00:00.000");
    CHECK(berlinTable.GetFormattedValue(0, 0) == "2026-01-15 13:00:00.000");
    CHECK(newYorkTable.GetFormattedValue(0, 0) == "2026-01-15 07:00:00.000");

    SECTION("A move keeps both zones")
    {
        berlinTable.SetDefaultSourceTimeZone(newYork);
        LogTable moved(std::move(berlinTable));
        CHECK(moved.DisplayTimeZone().IanaName() == "Europe/Berlin");
        CHECK(moved.DefaultSourceTimeZone().IanaName() == "America/New_York");
        CHECK(moved.GetFormattedValue(0, 0) == "2026-01-15 13:00:00.000");

        LogTable assigned;
        assigned = std::move(moved);
        CHECK(assigned.DisplayTimeZone().IanaName() == "Europe/Berlin");
        CHECK(assigned.DefaultSourceTimeZone().IanaName() == "America/New_York");
    }

    SECTION("Reset keeps both zones")
    {
        berlinTable.SetDefaultSourceTimeZone(newYork);
        berlinTable.Reset();
        CHECK(berlinTable.DisplayTimeZone().IanaName() == "Europe/Berlin");
        CHECK(berlinTable.DefaultSourceTimeZone().IanaName() == "America/New_York");
    }

    SECTION("A new table is UTC whatever the process zone is")
    {
        LogTable fresh;
        CHECK(fresh.DisplayTimeZone().IanaName() == TimeZoneContext{}.IanaName());
        CHECK(fresh.DefaultSourceTimeZone().IanaName() == TimeZoneContext{}.IanaName());
    }
}

TEST_CASE("A parse reads timestamps without an offset in the zone it is given", "[explicit_time_inputs]")
{
    // 2026-03-29 02:30 does not exist in Europe/Berlin: the clocks jump from 02:00 to 03:00.
    const std::string content = R"({"ts":"2026-03-29 02:30:00"})"
                                "\n";
    const std::int64_t wallClock = Micros(2026, 3, 29, 2, 30, 0);
    const TimeZoneContext berlin = TimeZoneContext::Load(FindTestTzdata(), "Europe/Berlin");

    ParserOptions berlinOptions;
    berlinOptions.timeColumns = {TimeColumn("ts", "%F %T")};
    berlinOptions.defaultSourceTimeZone = berlin;

    ParserOptions defaultOptions;
    defaultOptions.timeColumns = {TimeColumn("ts", "%F %T")};

    for (const char *processZone : {"Asia/Tokyo", "America/New_York"})
    {
        INFO("process zone: " << processZone);
        const ScopedProcessZone scoped(processZone);

        // The documented result of the zone's own conversion, whatever the process zone holds.
        CHECK(ParsedTimestampMicros(content, berlinOptions) == berlin.LocalMicrosecondsToUtc(wallClock));
        CHECK(berlin.LocalMicrosecondsToUtc(wallClock) != wallClock);

        // A default ParserOptions reads the text as UTC.
        CHECK(ParsedTimestampMicros(content, defaultOptions) == wallClock);
    }
}

TEST_CASE("A reference time fixes the year of a syslog timestamp for the whole parse", "[explicit_time_inputs]")
{
    const std::string content = "{\"ts\":\"Dec 31 23:59:59\"}\n"
                                "{\"ts\":\"Jan  1 00:00:00\"}\n";

    const auto parseYears = [&content](std::chrono::system_clock::time_point reference) {
        const TestLogFile file("explicit_time_reference.json");
        file.Write(content);
        auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(file.GetFilePath()));
        FileLineSource *sourcePtr = source.get();
        internal::BufferingSink sink(std::move(source));
        ParserOptions options;
        options.timeColumns = {TimeColumn("ts", "%b %e %H:%M:%S")};
        options.referenceTime = reference;
        JsonParser{}.ParseStreaming(*sourcePtr, sink, options);
        const LogData data = sink.TakeData();
        std::vector<std::int64_t> micros;
        for (const LogLine &line : data.Lines())
        {
            const LogValue value = line.GetValue("ts");
            const auto *stamp = std::get_if<TimeStamp>(&value);
            REQUIRE(stamp != nullptr);
            micros.push_back(stamp->time_since_epoch().count());
        }
        return micros;
    };

    const auto at = [](int year, unsigned month, unsigned day) {
        return std::chrono::system_clock::time_point{
            std::chrono::sys_days{std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}}
        };
    };

    // In mid-January, December belongs to the year before and January to this year.
    const auto january = parseYears(at(2026, 1, 15));
    REQUIRE(january.size() == 2);
    CHECK(january[0] == Micros(2025, 12, 31, 23, 59, 59));
    CHECK(january[1] == Micros(2026, 1, 1, 0, 0, 0));

    // The same lines in the same parse use the same reference, so a later clock does not change them.
    const auto december = parseYears(at(2026, 12, 31));
    REQUIRE(december.size() == 2);
    CHECK(december[0] == Micros(2026, 12, 31, 23, 59, 59));
    CHECK(december[1] == Micros(2026, 1, 1, 0, 0, 0));
}

TEST_CASE("Feb 29 in a syslog timestamp parses with a leap-year reference only", "[explicit_time_inputs]")
{
    TimeStamp out{};
    const auto at = [](int year, unsigned month, unsigned day) {
        return std::chrono::system_clock::time_point{
            std::chrono::sys_days{std::chrono::year{year} / std::chrono::month{month} / std::chrono::day{day}}
        };
    };
    CHECK(TryParseSyslogRfc3164Timestamp("Feb 29 12:00:00", out, at(2024, 6, 1)));
    CHECK(out.time_since_epoch().count() == Micros(2024, 2, 29, 12, 0, 0));
    CHECK_FALSE(TryParseSyslogRfc3164Timestamp("Feb 29 12:00:00", out, at(2025, 6, 1)));
}

TEST_CASE("StreamLineSource hands unread bytes back in front of the producer's bytes", "[explicit_time_inputs]")
{
    StreamLineSource source(std::filesystem::path("unread.log"), std::make_unique<StringProducer>("{\"n\":3}\n"));

    SECTION("TakeUnreadBytes returns them in order and empties them")
    {
        source.UnreadBytes("second");
        source.UnreadBytes("first-");
        CHECK(source.TakeUnreadBytes() == "first-second");
        CHECK(source.TakeUnreadBytes().empty());
    }

    SECTION("A parse processes them before the producer's bytes")
    {
        source.UnreadBytes("{\"n\":2}\n");
        source.UnreadBytes("{\"n\":1}\n");

        CollectingSink sink;
        JsonParser{}.ParseStreaming(source, sink, ParserOptions{});
        REQUIRE(sink.finished);
        REQUIRE(sink.Errors().empty());

        const auto lines = sink.Lines();
        REQUIRE(lines.size() == 3);
        for (std::size_t i = 0; i < lines.size(); ++i)
        {
            const LogValue value = lines[i]->GetValue("n");
            const auto *number = std::get_if<std::int64_t>(&value);
            REQUIRE(number != nullptr);
            CHECK(*number == static_cast<std::int64_t>(i + 1));
        }
        CHECK(source.TakeUnreadBytes().empty());
    }

    SECTION("AutoDetectParser keeps the bytes the caller gave back")
    {
        source.UnreadBytes("{\"n\":2}\n");
        source.UnreadBytes("{\"n\":1}\n");

        CollectingSink sink;
        AutoDetectParser(PROBE_BYTES_BUDGET, std::chrono::milliseconds{0})
            .ParseStreaming(source, sink, ParserOptions{});
        REQUIRE(sink.finished);

        const auto lines = sink.Lines();
        REQUIRE(lines.size() == 3);
        const LogValue first = lines[0]->GetValue("n");
        const LogValue last = lines[2]->GetValue("n");
        REQUIRE(std::get_if<std::int64_t>(&first) != nullptr);
        REQUIRE(std::get_if<std::int64_t>(&last) != nullptr);
        CHECK(*std::get_if<std::int64_t>(&first) == 1);
        CHECK(*std::get_if<std::int64_t>(&last) == 3);
    }
}

TEST_CASE("The logfmt continuation switch belongs to the parser", "[explicit_time_inputs]")
{
    const std::string payload = "level=info msg=\"first record\"\n"
                                "\ttab-indented continuation\n";

    const auto rowCount = [&payload](const LogParser &parser) {
        StreamLineSource source(std::filesystem::path("logfmt.log"), std::make_unique<StringProducer>(payload));
        CollectingSink sink;
        parser.ParseStreaming(source, sink, ParserOptions{});
        REQUIRE(sink.finished);
        return sink.Lines().size();
    };

    CHECK(rowCount(LogfmtParser{}) == 1);
    CHECK(rowCount(LogfmtParser{true}) == 1);
    CHECK(rowCount(LogfmtParser{false}) == 2);
    CHECK(rowCount(*MakeParserForFormat(Source::Format::Logfmt, {})) == 1);
    CHECK(rowCount(*MakeParserForFormat(Source::Format::Logfmt, {.multilineLogfmt = false})) == 2);
    CHECK(
        rowCount(AutoDetectParser(PROBE_BYTES_BUDGET, std::chrono::milliseconds{0}, {.multilineLogfmt = false})) == 2
    );
}

TEST_CASE("A saved regex source restores its multi-line parse without the template", "[explicit_time_inputs]")
{
    // The pattern is deliberately not in any template registry.
    const std::string pattern = R"(^(?<level>\w+)\s+(?<message>.*)$)";
    const std::string payload = "error boom\n"
                                "\tat Foo.bar\n"
                                "\tat Baz.qux\n"
                                "info recovered\n";

    Source saved;
    saved.kind = Source::Kind::NetworkStream;
    saved.format = Source::Format::Regex;
    saved.locators = {{"tcp://example", "tcp://example"}};
    saved.regexPattern = pattern;
    saved.regexContinuationMode = ContinuationMode::Indented;

    LogConfiguration configuration;
    configuration.source = saved;
    const TestLogFile configFile("explicit_time_source.json");
    LogConfigurationManager::Save(configuration, configFile.GetFilePath(), SaveScope::Full);

    LogConfigurationManager loaded;
    loaded.Load(configFile.GetFilePath());
    REQUIRE(loaded.Configuration().source.has_value());
    const Source &restored = *loaded.Configuration().source;
    CHECK(restored.regexContinuationMode == ContinuationMode::Indented);
    CHECK(restored.regexHeaderAnchor.empty());
    CHECK_FALSE(FindTemplateByPattern(restored.regexPattern).has_value());

    const RegexParser parser{restored.regexPattern, restored.regexContinuationMode, restored.regexHeaderAnchor};
    StreamLineSource source(std::filesystem::path("regex.log"), std::make_unique<StringProducer>(payload));
    CollectingSink sink;
    parser.ParseStreaming(source, sink, ParserOptions{});
    REQUIRE(sink.finished);
    CHECK(sink.Errors().empty());
    CHECK(sink.Lines().size() == 2);
}

TEST_CASE("Detection of a multi-line template reports its continuation settings", "[explicit_time_inputs]")
{
    const std::string sample = "2024-04-28 04:02:03,123 ERROR [main] boom\n"
                               "Traceback (most recent call last):\n"
                               "  File \"a.py\", line 7\n"
                               "2024-04-28 04:02:04,001 INFO [main] recovered\n"
                               "2024-04-28 04:02:05,001 INFO [main] again\n";
    const auto detected = TryDetectFormatFromBytes(sample);
    if (!detected.has_value() || detected->format != Source::Format::Regex)
    {
        SKIP("No shipped template matches this sample.");
    }
    const auto tmpl = FindTemplateByPattern(detected->regexPattern);
    REQUIRE(tmpl.has_value());
    CHECK(detected->regexContinuationMode == tmpl->continuationMode);
    CHECK(detected->regexHeaderAnchor == tmpl->headerAnchor);

    const FormatOptions options = detected->Options(false);
    CHECK(options.regexPattern == detected->regexPattern);
    CHECK(options.regexContinuationMode == detected->regexContinuationMode);
    CHECK_FALSE(options.multilineLogfmt);
}

TEST_CASE("A print format that no value accepts is replaced on load and never throws", "[explicit_time_inputs]")
{
    LogConfigurationManager manager;
    manager.LoadFromString(R"({"columns":[
        {"header":"latency","keys":["latency"],"printFormat":"{:q}","type":"any","parseFormats":[],"visible":true},
        {"header":"ok","keys":["ok"],"printFormat":"{:.1f}","type":"any","parseFormats":[],"visible":true}
    ]})");

    const auto &columns = manager.Configuration().columns;
    REQUIRE(columns.size() == 2);
    CHECK(columns[0].printFormat == "{}");
    CHECK(columns[1].printFormat == "{:.1f}");

    const std::vector<std::string> warnings = manager.TakeWarnings();
    REQUIRE(warnings.size() == 1);
    CHECK(warnings[0].contains("latency"));
    CHECK(warnings[0].contains("{:q}"));
    CHECK(manager.TakeWarnings().empty());

    SECTION("A format set on a column later is checked too")
    {
        manager.SetColumnPrintFormat(1, "{:q}");
        CHECK(manager.Configuration().columns[1].printFormat == "{}");
        CHECK(manager.TakeWarnings().size() == 1);
    }

    SECTION("A format that only some value types accept falls back to plain text for the others")
    {
        manager.SetColumnPrintFormat(1, "{:d}");
        REQUIRE(manager.TakeWarnings().empty());
        const TestLogFile file("explicit_time_print.json");
        file.Write("{}\n");
        auto source = file.CreateFileLineSource();
        FileLineSource *sourcePtr = source.get();
        KeyIndex keys;
        std::vector<LogLine> lines;
        lines.emplace_back(LogMap{{"ok", 1.5}}, keys, *sourcePtr, 0);
        LogData data(std::move(source), std::move(lines), std::move(keys));
        data.MarkTimestampsParsed();
        LogTable table(std::move(data), std::move(manager));
        REQUIRE(table.Configuration().Configuration().columns[1].printFormat == "{:d}");
        // The "ok" column is the second one, but the table finds cells by key, so ask for its index.
        CHECK_NOTHROW(table.GetFormattedValue(0, 1));
        CHECK(table.GetFormattedValue(0, 1) == "1.5");
    }
}
