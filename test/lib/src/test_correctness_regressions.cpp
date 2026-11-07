#include "common.hpp"
#include "parse_only.hpp"
#include "table_fixture.hpp"
#include "table_from_data.hpp"
#include <loglib/core/bytes_producer.hpp>
#include <loglib/core/error.hpp>
#include <loglib/core/file_line_source.hpp>
#include <loglib/core/internal/stable_byte_arena.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/ingest/decompressing_byte_source.hpp>
#include <loglib/ingest/tailing_bytes_producer.hpp>
#include <loglib/output/exports/export_sink.hpp>
#include <loglib/output/session_bundle.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/parse/log_parser.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/parse/parsers/logfmt_parser.hpp>
#include <loglib/query/histogram_bucket_index.hpp>
#include <loglib/query/leaf_rule_compile.hpp>
#include <loglib/query/log_filter.hpp>
#include <loglib/query/query_parser.hpp>
#include <loglib/schema/log_configuration.hpp>

#include <loglib/table/log_table.hpp>

#include <test_common/temp_dir.hpp>
#include <test_common/wait.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

using namespace loglib;
using namespace std::chrono_literals;

namespace
{

/// Applies each batch to the file the way a consumer does, one flush at a time.
class CoalescingFileSink : public LogParseSink
{
public:
    explicit CoalescingFileSink(FileLineSource &file)
        : mFile(&file)
    {
    }

    KeyIndex &Keys() override
    {
        return mKeys;
    }
    void OnStarted() override
    {
    }
    void OnBatch(RowBatch batch) override
    {
        mFile->AppendLineOffsets(batch.localLineOffsets);
        for (const MultiLineRecordSpan &span : batch.multiLineSpans)
        {
            mFile->RegisterMultiLineRecord(span.headerLineId, span.lastLineId);
        }
        std::ranges::move(batch.lines, std::back_inserter(lines));
    }
    void OnFinished(bool /*cancelled*/) override
    {
    }

    std::vector<LogLine> lines;

private:
    FileLineSource *mFile;
    KeyIndex mKeys;
};

std::vector<std::string> RawLinesOfParse(const std::filesystem::path &path, std::size_t batchSizeBytes)
{
    FileLineSource source(path);
    CoalescingFileSink sink(source);
    ParserOptions options;
    options.batchSizeBytes = batchSizeBytes;
    LogfmtParser{}.ParseStreaming(source, sink, options);

    std::vector<std::string> raw;
    raw.reserve(sink.lines.size());
    for (const LogLine &line : sink.lines)
    {
        raw.push_back(source.RawLine(line.LineId()));
    }
    return raw;
}

ParseResult ParseText(const test_common::TempDir &dir, const LogParser &parser, std::string_view text)
{
    const std::filesystem::path path = dir.Write("input.log", text);
    auto source = std::make_unique<FileLineSource>(path);
    FileLineSource &sourceRef = *source;
    internal::BufferingSink sink(std::move(source));
    parser.ParseStreaming(sourceRef, sink, ParserOptions{});
    LogData data = sink.TakeData();
    return ParseResult{.data = std::move(data), .errors = sink.TakeErrors()};
}

LogTable TableOf(LogData data)
{
    return test_common_lib::TableFromData(std::move(data), LogConfigurationManager{});
}

class CollectingSink : public LogParseSink
{
public:
    KeyIndex &Keys() override
    {
        return keys;
    }
    void OnStarted() override
    {
    }
    void OnBatch(RowBatch batch) override
    {
        std::ranges::move(batch.lines, std::back_inserter(lines));
        std::ranges::move(batch.errors, std::back_inserter(errors));
    }
    void OnFinished(bool /*cancelled*/) override
    {
    }

    KeyIndex keys;
    std::vector<LogLine> lines;
    std::vector<loglib::ParseError> errors;
};

using test_common::WaitUntil;

std::string DrainTo(TailingBytesProducer &producer, std::string &collected)
{
    std::array<char, 4096> buffer{};
    for (size_t got = producer.Read(buffer); got != 0; got = producer.Read(buffer))
    {
        collected.append(buffer.data(), got);
    }
    return collected;
}

} // namespace

TEST_CASE("StableByteArena keeps appended bytes readable and in place", "[stable_byte_arena]")
{
    internal::StableByteArena arena;
    CHECK(arena.View(0, 1).empty());

    const uint64_t first = arena.Append("first");
    const uint64_t second = arena.Append("second");
    const std::string_view firstView = arena.View(first, 5);
    CHECK(firstView == "first");
    CHECK(arena.View(second, 6) == "second");
    CHECK(arena.View(second, 7).empty());

    const std::string large(3 * 1024 * 1024, 'x');
    const uint64_t largeOffset = arena.Append(large);
    CHECK(arena.View(largeOffset, large.size()) == large);
    // Growth never moves earlier bytes.
    CHECK(firstView.data() == arena.View(first, 5).data());
    CHECK(firstView == "first");
}

TEST_CASE("StableByteArena extends only the most recent range", "[stable_byte_arena]")
{
    internal::StableByteArena arena;
    const uint64_t first = arena.Append("ab");
    CHECK(arena.TryExtend(first, 2, "cd"));
    CHECK(arena.View(first, 4) == "abcd");

    const uint64_t second = arena.Append("z");
    CHECK_FALSE(arena.TryExtend(first, 4, "e"));
    CHECK(arena.TryExtend(second, 1, "y"));
    CHECK(arena.View(second, 2) == "zy");
}

TEST_CASE("StableByteArena serves readers while another thread appends", "[stable_byte_arena][threads]")
{
    internal::StableByteArena arena;
    constexpr size_t COUNT = 20000;
    std::vector<std::pair<uint64_t, std::string>> published(COUNT);
    std::atomic<size_t> ready{0};

    std::thread writer([&] {
        for (size_t i = 0; i < COUNT; ++i)
        {
            // Long enough to cross block boundaries many times.
            std::string text(200 + (i % 500), static_cast<char>('a' + (i % 26)));
            const uint64_t offset = arena.Append(text);
            published[i] = {offset, std::move(text)};
            ready.store(i + 1, std::memory_order_release);
        }
    });

    size_t mismatches = 0;
    for (size_t seen = 0; seen < COUNT;)
    {
        const size_t limit = ready.load(std::memory_order_acquire);
        for (; seen < limit; ++seen)
        {
            if (arena.View(published[seen].first, published[seen].second.size()) != published[seen].second)
            {
                ++mismatches;
            }
        }
    }
    writer.join();
    CHECK(mismatches == 0);
}

TEST_CASE("Raw lines stay aligned when batches contain no records", "[parser_pipeline][raw_lines]")
{
    const test_common::TempDir dir("raw_lines");
    std::string text = "id=0 msg=first\n";
    for (int i = 0; i < 40; ++i)
    {
        text += "bad=\"unterminated " + std::to_string(i) + "\n";
    }
    text += "id=1 msg=after\n  continued\nbad=\"x\nid=2 msg=last\n";
    const std::filesystem::path path = dir.Write("mixed.logfmt", text);

    const std::vector<std::string> expected = {"id=0 msg=first", "id=1 msg=after\n  continued", "id=2 msg=last"};
    const std::size_t batchSize = GENERATE(16U, 64U, 128U, 256U, 1024U, 65536U);
    INFO("batchSizeBytes = " << batchSize);
    CHECK(RawLinesOfParse(path, batchSize) == expected);
}

TEST_CASE("Malformed JSON values fail the line instead of dropping fields", "[json_parser][malformed]")
{
    const test_common::TempDir dir("json");
    const ParseResult result = ParseText(
        dir,
        JsonParser{},
        "{\"a\":1,\"b\":2}\n"
        "{\"a\":1,\"b\":truex}\n"
        "{\"a\":1,,\"b\":2}\n"
        "{\"a\":1,\"b\":nul}\n"
        "{\"a\":1,\"b\":null}\n"
    );
    CHECK(result.data.Lines().size() == 2);
    REQUIRE(result.errors.size() == 3);
    CHECK(loglib::FormatParseError(result.errors[0]).starts_with("Error on line 2:"));
    CHECK(loglib::FormatParseError(result.errors[1]).starts_with("Error on line 3:"));
    CHECK(loglib::FormatParseError(result.errors[2]).starts_with("Error on line 4:"));
}

TEST_CASE("Malformed JSON values fail the line in a live stream", "[json_parser][malformed][stream]")
{
    // The first line caches each key's type, so later lines cover both decode paths.
    StreamLineSource source(
        "<test>",
        std::make_unique<InMemoryBytesProducer>("{\"b\":true,\"s\":\"ok\",\"n\":1}\n"
                                                "{\"b\":truex}\n"
                                                "{\"s\":\"bad\\q\"}\n"
                                                "{\"fresh\":truex}\n"
                                                "{\"other\":\"bad\\q\"}\n"
                                                "{\"num\":1e999}\n"
                                                "{\"null\":nul}\n"
                                                "{\"b\":false,\"s\":\"fine\",\"n\":2}\n")
    );
    CollectingSink sink;
    JsonParser{}.ParseStreaming(source, sink, ParserOptions{});

    CHECK(sink.lines.size() == 2);
    REQUIRE(sink.errors.size() == 6);
    CHECK(loglib::FormatParseError(sink.errors[0]).starts_with("Error on line 2:"));
    CHECK(loglib::FormatParseError(sink.errors[5]).starts_with("Error on line 7:"));
}

TEST_CASE("JSON integers past 64 bits keep their text", "[json_parser][malformed]")
{
    const test_common::TempDir dir("json");
    const ParseResult result = ParseText(dir, JsonParser{}, "{\"id\":123456789012345678901234567890}\n");
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == 1);
    const LogValue value = result.data.Lines()[0].GetValue(result.data.Keys(), "id");
    const auto text = AsStringView(value);
    REQUIRE(text.has_value());
    CHECK(*text == "123456789012345678901234567890");
}

TEST_CASE("Bare scalars that a number cannot represent stay strings", "[logfmt_parser][scalars]")
{
    const test_common::TempDir dir("scalars");
    const ParseResult result = ParseText(
        dir,
        LogfmtParser{},
        "zip=007 big=18446744073709551616 neg=-007 small=-99999999999999999999 plus=+5 n=42 f=1.5 z=0\n"
    );
    REQUIRE(result.data.Lines().size() == 1);
    const LogLine &line = result.data.Lines()[0];
    const auto text = [&line, &result](const char *key) {
        const LogValue value = line.GetValue(result.data.Keys(), key);
        return std::string(AsStringView(value).value_or("<not a string>"));
    };
    CHECK(text("zip") == "007");
    CHECK(text("big") == "18446744073709551616");
    CHECK(text("neg") == "-007");
    CHECK(text("small") == "-99999999999999999999");
    CHECK(text("plus") == "+5");
    CHECK(std::holds_alternative<uint64_t>(line.GetValue(result.data.Keys(), "n")));
    CHECK(std::holds_alternative<double>(line.GetValue(result.data.Keys(), "f")));
    CHECK(std::holds_alternative<uint64_t>(line.GetValue(result.data.Keys(), "z")));
}

TEST_CASE("Logfmt strips a leading BOM", "[logfmt_parser][bom]")
{
    const test_common::TempDir dir("bom");
    const ParseResult result = ParseText(dir, LogfmtParser{}, "\xEF\xBB\xBFts=1 msg=a\nts=2 msg=b\n");
    CHECK(result.data.Keys().Find("ts") != INVALID_KEY_ID);
    CHECK(result.data.Keys().Find("\xEF\xBB\xBFts") == INVALID_KEY_ID);

    StreamLineSource source("<test>", std::make_unique<InMemoryBytesProducer>("\xEF\xBB\xBFts=1 msg=a\nts=2 msg=b\n"));
    CollectingSink sink;
    LogfmtParser{}.ParseStreaming(source, sink, ParserOptions{});
    CHECK(sink.lines.size() == 2);
    CHECK(sink.keys.Find("\xEF\xBB\xBFts") == INVALID_KEY_ID);
}

TEST_CASE("ParseQuery rejects excessive nesting instead of overflowing the stack", "[query_parser][nesting]")
{
    const auto nested = [](std::size_t depth) { return std::string(depth, '(') + "n>1" + std::string(depth, ')'); };
    CHECK(ParseQuery(nested(MAX_QUERY_NESTING), TimeZoneContext{}).has_value());
    CHECK_FALSE(ParseQuery(nested(MAX_QUERY_NESTING + 1), TimeZoneContext{}).has_value());
    CHECK_FALSE(ParseQuery(nested(200000), TimeZoneContext{}).has_value());

    std::string notChain;
    for (int i = 0; i < 200000; ++i)
    {
        notChain += "NOT ";
    }
    CHECK_FALSE(ParseQuery(notChain + "n>1", TimeZoneContext{}).has_value());
}

TEST_CASE("A level selection with no matching rows matches nothing", "[leaf_rule_compile][level]")
{
    const test_common::TempDir dir("level");
    ParseResult result =
        ParseText(dir, JsonParser{}, "{\"level\":\"info\"}\n{\"level\":\"warn\"}\n{\"level\":\"info\"}\n");
    const LogTable table = TableOf(std::move(result.data));
    const auto &columns = table.Configuration().Configuration().columns;
    REQUIRE(columns.at(0).type == ColumnType::Level);

    const auto rowsFor = [&](const char *query) {
        const auto expression = ParseQuery(query, TimeZoneContext{});
        REQUIRE(expression.has_value());
        return FilterAcceptedRows(table, CompileExpression(*expression, columns, &table)).size();
    };
    CHECK(rowsFor("level IN [Trace]") == 0);
    CHECK(rowsFor("level IN [Warn]") == 1);
    CHECK(rowsFor("NOT level IN [Trace]") == 3);
}

TEST_CASE("FindFilterProblems reports unknown columns and invalid patterns", "[leaf_rule_compile][problems]")
{
    const std::vector<Column> columns = {Column{.header = "msg", .keys = {"msg"}}};
    const auto problemsFor = [&](const char *query) {
        const auto expression = ParseQuery(query, TimeZoneContext{});
        REQUIRE(expression.has_value());
        return FindFilterProblems(*expression, columns);
    };
    CHECK(problemsFor("msg:ok").empty());
    CHECK(problemsFor("msg~/ok/").empty());
    CHECK(problemsFor("nosuch:x").size() == 1);
    CHECK(problemsFor("NOT nosuch:x").size() == 1);
    CHECK(problemsFor("msg~/(/").size() == 1);
    CHECK(problemsFor("msg:a OR (nosuch:x AND msg~/(/)").size() == 2);
}

TEST_CASE("Evicting rows trims each source by its own line ids", "[log_table][retention]")
{
    const test_common::TempDir dir("evict");
    const std::filesystem::path path = dir.Write("history.log", "unused\n");
    auto fileSource = std::make_unique<FileLineSource>(path);
    FileLineSource &fileRef = *fileSource;

    LogTable table;
    table.BeginStreaming(std::move(fileSource));
    KeyIndex &keys = table.Keys();
    const KeyId valueKey = keys.GetOrInsert("value");

    const auto makeLine = [&](LineSource &source, size_t lineId, const char *text) {
        std::vector<std::pair<KeyId, LogValue>> values;
        values.emplace_back(valueKey, LogValue{std::string(text)});
        return LogLine(std::move(values), source, lineId);
    };

    RowBatch fileBatch;
    fileBatch.newKeys.emplace_back("value");
    // File line ids are far past anything the stream source has assigned.
    fileBatch.lines.push_back(makeLine(fileRef, 5000, "file-a"));
    fileBatch.lines.push_back(makeLine(fileRef, 5001, "file-b"));
    AppendFixtureBatch(table, std::move(fileBatch));

    auto streamSource = std::make_unique<StreamLineSource>("<stream>", nullptr);
    StreamLineSource &streamRef = *streamSource;
    table.AppendStreaming(std::move(streamSource));
    RowBatch streamBatch;
    for (const char *text : {"stream-a", "stream-b"})
    {
        const size_t lineId = streamRef.AppendLine(text, "");
        streamBatch.lines.push_back(makeLine(streamRef, lineId, text));
    }
    AppendFixtureBatch(table, std::move(streamBatch));

    // The first survivor is a file row: stream rows must keep their bytes.
    table.EvictPrefixRows(1);
    REQUIRE(table.RowCount() == 3);
    CHECK(streamRef.Size() == 2);
    CHECK(std::string(*AsStringView(table.GetValue(1, 0))) == "stream-a");
    CHECK(streamRef.RawLine(1) == "stream-a");

    // Now the evicted rows reach the stream: only its own evicted line goes.
    table.EvictPrefixRows(2);
    REQUIRE(table.RowCount() == 1);
    CHECK(streamRef.Size() == 1);
    CHECK(std::string(*AsStringView(table.GetValue(0, 0))) == "stream-b");
    CHECK(streamRef.RawLine(2) == "stream-b");
}

TEST_CASE("An outlier timestamp widens histogram buckets instead of allocating", "[histogram][outlier]")
{
    const auto at = [](int64_t seconds) { return TimeStamp{std::chrono::seconds{seconds}}; };
    constexpr int64_t YEAR = 31'536'000;
    constexpr int64_t BASE = 56 * YEAR;

    HistogramBucketIndex index(HistogramBucketSize::OneSecond);
    index.AddRow(at(BASE), LogLevel::Info);
    index.AddRow(at(BASE + 1), LogLevel::Warn);
    CHECK(index.BucketSize() == HistogramBucketSize::OneSecond);

    index.AddRow(at(BASE + (2 * YEAR)), LogLevel::Error);
    CHECK(index.BucketSize() != HistogramBucketSize::OneSecond);
    CHECK(index.Buckets().size() <= HISTOGRAM_MAX_BUCKETS);
    CHECK(index.TotalRowCount() == 3);
    uint64_t total = 0;
    for (const LevelBucket &bucket : index.Buckets())
    {
        total += bucket.Total();
    }
    CHECK(total == 3);
    CHECK(index.Buckets().front().counts[static_cast<size_t>(LogLevel::Warn)] == 1);

    // Backward outliers widen too, and the rung stops at one day.
    index.AddRow(at(-1000 * YEAR), LogLevel::Info);
    index.AddRow(at(100000 * YEAR), LogLevel::Info);
    CHECK(index.BucketSize() == HistogramBucketSize::OneDay);
    CHECK(index.Buckets().size() <= HISTOGRAM_MAX_BUCKETS);
    total = 0;
    for (const LevelBucket &bucket : index.Buckets())
    {
        total += bucket.Total();
    }
    CHECK(total == 5);
}

TEST_CASE("The stream loop drops an oversized line and resumes after it", "[stream][limits]")
{
    std::string bytes = "a=1\n" + std::string(100000, 'x') + "\na=2\n";
    StreamLineSource source("<test>", std::make_unique<InMemoryBytesProducer>(std::move(bytes)));
    CollectingSink sink;
    ParserOptions options;
    options.maxLineBytes = 1024;
    LogfmtParser{}.ParseStreaming(source, sink, options);

    CHECK(sink.lines.size() == 2);
    REQUIRE(sink.errors.size() == 1);
    CHECK(loglib::FormatParseError(sink.errors[0]).starts_with("Error on line 2:"));
    CHECK(source.RawLine(2) == "a=2");
}

TEST_CASE("The stream loop bounds a record's continuation bytes", "[stream][limits]")
{
    std::string bytes = "a=first\n";
    for (int i = 0; i < 1000; ++i)
    {
        bytes += "  continuation line\n";
    }
    bytes += "a=second\n";
    StreamLineSource source("<test>", std::make_unique<InMemoryBytesProducer>(std::move(bytes)));
    CollectingSink sink;
    ParserOptions options;
    options.maxLineBytes = 1024;
    LogfmtParser{}.ParseStreaming(source, sink, options);

    CHECK(sink.lines.size() == 2);
    CHECK(source.RawLine(1).size() <= 2 * options.maxLineBytes);
    REQUIRE(sink.errors.size() == 1);
    CHECK(loglib::FormatParseError(sink.errors[0]).find("Continuation lines dropped") != std::string::npos);
}

TEST_CASE("Tailing delivers bytes written before a rename rotation", "[TailingBytesProducer][rotation]")
{
    const test_common::TempDir dir("rotate");
    const std::filesystem::path path = dir.Write("app.log", "one\n");

    TailingBytesProducer::Options options;
    options.disableNativeWatcher = true;
    options.pollInterval = 200ms;
    TailingBytesProducer producer(path, 10, options);

    std::string collected;
    REQUIRE(WaitUntil([&] { return DrainTo(producer, collected).contains("one\n"); }));

    // All within one poll interval, so the worker first sees the new file.
    {
        std::ofstream append(path, std::ios::binary | std::ios::app);
        append << "two\nthree-without-newline";
    }
    std::filesystem::rename(path, dir.Path() / "app.log.1");
    (void)dir.Write("app.log", "four\n");

    REQUIRE(WaitUntil([&] { return DrainTo(producer, collected).contains("four\n"); }));
    CHECK(collected.contains("two\n"));
    CHECK(collected.contains("three-without-newline\n"));
    producer.Stop();
}

TEST_CASE("Tailing drops an oversized line and resumes after it", "[TailingBytesProducer][limits]")
{
    const test_common::TempDir dir("tail_limit");
    const std::filesystem::path path = dir.Write("app.log", "start\n");

    TailingBytesProducer::Options options;
    options.disableNativeWatcher = true;
    options.pollInterval = 10ms;
    options.readChunkBytes = 1024;
    options.maxLineBytes = 1024;
    TailingBytesProducer producer(path, 10, options);

    // Appending after pre-fill keeps the long line out of the one big pre-fill read.
    std::string collected;
    REQUIRE(WaitUntil([&] { return DrainTo(producer, collected).contains("start\n"); }));
    {
        std::ofstream append(path, std::ios::binary | std::ios::app);
        append << std::string(5000, 'x') << "\nnext\n";
    }
    REQUIRE(WaitUntil([&] { return DrainTo(producer, collected).contains("next\n"); }));
    CHECK(collected == "start\nnext\n");
    CHECK(producer.DroppedByteCount() > 0);
    producer.Stop();
}

TEST_CASE("Tailing pauses reading while the queue is full and loses nothing", "[TailingBytesProducer][limits]")
{
    const test_common::TempDir dir("tail_queue");
    const std::filesystem::path path = dir.Write("app.log", "start\n");

    TailingBytesProducer::Options options;
    options.disableNativeWatcher = true;
    options.pollInterval = 5ms;
    options.readChunkBytes = 1024;
    options.maxQueuedBytes = 4096;
    TailingBytesProducer producer(path, 10, options);

    // Pre-fill keeps only the last lines, so append after it has run.
    std::string collected;
    REQUIRE(WaitUntil([&] { return DrainTo(producer, collected).contains("start\n"); }));

    std::string appended;
    for (int i = 0; i < 2000; ++i)
    {
        appended += "line " + std::to_string(i) + "\n";
    }
    {
        std::ofstream append(path, std::ios::binary | std::ios::app);
        append << appended;
    }
    const std::string expected = "start\n" + appended;
    REQUIRE(WaitUntil([&] { return DrainTo(producer, collected).size() >= expected.size(); }));
    CHECK(collected == expected);
    producer.Stop();
}

TEST_CASE("Tailing does not read a rotated file past the queue limit", "[TailingBytesProducer][limits][rotation]")
{
    const test_common::TempDir dir("tail_rotate_queue");
    const std::filesystem::path path = dir.Write("app.log", "start\n");

    TailingBytesProducer::Options options;
    options.disableNativeWatcher = true;
    options.pollInterval = 5ms;
    options.readChunkBytes = 1024;
    options.maxQueuedBytes = 4096;
    TailingBytesProducer producer(path, 10, options);

    std::string collected;
    REQUIRE(WaitUntil([&] { return DrainTo(producer, collected).contains("start\n"); }));

    std::string appended;
    for (int i = 0; i < 2000; ++i)
    {
        appended += "line " + std::to_string(i) + "\n";
    }
    {
        std::ofstream append(path, std::ios::binary | std::ios::app);
        append << appended;
    }
    std::filesystem::rename(path, dir.Path() / "app.log.1");
    (void)dir.Write("app.log", "after\n");

    // With nothing consumed, the drain stays parked and rotation cannot finish.
    std::this_thread::sleep_for(200ms);
    CHECK(producer.RotationCount() == 0);

    const std::string expected = "start\n" + appended + "after\n";
    REQUIRE(WaitUntil([&] { return DrainTo(producer, collected).size() >= expected.size(); }, 60s));
    CHECK(collected == expected);
    CHECK(producer.RotationCount() >= 1);
    producer.Stop();
}

TEST_CASE("Tailing keeps a partial line while a rotated path is missing", "[TailingBytesProducer][rotation]")
{
    const test_common::TempDir dir("tail_rotate_partial");
    const std::filesystem::path path = dir.Write("app.log", "start\n");
    const std::filesystem::path rotated = dir.Path() / "app.log.1";

    TailingBytesProducer::Options options;
    options.disableNativeWatcher = true;
    options.pollInterval = 5ms;
    TailingBytesProducer producer(path, 10, options);

    std::string collected;
    REQUIRE(WaitUntil([&] { return DrainTo(producer, collected).contains("start\n"); }, 60s));

    const auto append = [](const std::filesystem::path &target, std::string_view text) {
        std::ofstream output(target, std::ios::binary | std::ios::app);
        output << text;
    };
    append(path, "first half");
    std::this_thread::sleep_for(100ms);

    // The worker sees the path missing while its handle can still read the file.
    std::filesystem::rename(path, rotated);
    std::this_thread::sleep_for(100ms);
    append(rotated, ", second half\n");
    std::this_thread::sleep_for(100ms);
    (void)dir.Write("app.log", "after\n");

    REQUIRE(WaitUntil([&] { return DrainTo(producer, collected).contains("after\n"); }, 60s));
    CHECK(collected == "start\nfirst half, second half\nafter\n");
    producer.Stop();
}

TEST_CASE("Session bundle metadata rejects a very deep filter tree", "[session_bundle][nesting]")
{
    constexpr int DEPTH = 100000;
    std::string tree;
    for (int i = 0; i < DEPTH; ++i)
    {
        tree += R"({"kind":"not","child":)";
    }
    tree += R"({"kind":"leaf","rule":{"type":"string","columnKeys":["n"],"matchType":"contains","filterString":"1"}})";
    tree += std::string(DEPTH, '}');
    const std::string json = R"({"bundle":{"configuration":{"expression":)" + tree + "}}}";
    CHECK_THROWS_AS(ParseSessionBundleMetadata(json), FormatError);
}

TEST_CASE("File APIs accept paths outside the active code page", "[paths][unicode]")
{
    const test_common::TempDir dir("unicode");
    const std::filesystem::path path = dir.Path() / std::filesystem::path(u8"日本語.log");
    {
        std::ofstream out(path, std::ios::binary);
        out << "a=1\n";
    }

    CHECK(FileLineSource(path).StableBytes().size() == 4);
    CHECK(DecompressingByteSource(path).EffectivePath() == path);
    CHECK_NOTHROW(TailingBytesProducer(path, 10));

    const std::filesystem::path config = dir.Path() / std::filesystem::path(u8"設定.json");
    const LogConfigurationManager manager;
    CHECK_NOTHROW(manager.Save(config));
    CHECK(std::filesystem::exists(config));

    const std::filesystem::path exported = dir.Path() / std::filesystem::path(u8"出力.log");
    {
        exports::FileSink sink(exported);
        sink.Write("x");
        CHECK_NOTHROW(sink.Finish());
    }
    CHECK(std::filesystem::exists(exported));

    // Error messages must not throw while being built.
    const std::filesystem::path missing = dir.Path() / std::filesystem::path(u8"無.log");
    CHECK_THROWS_WITH(DecompressingByteSource(missing), Catch::Matchers::ContainsSubstring("does not exist"));
}
