// Parser benchmarks for `loglib`. See CONTRIBUTING.md `## Benchmarking`
// for the PR-process docs (regression gate, sample interpretation, how
// to run). Debug builds skip these cases automatically — see
// `BENCHMARK_REQUIRES_RELEASE_BUILD`.

#include "benchmark_common.hpp"
#include "common.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/internal/advanced_parser_options.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_parser.hpp>
#include <loglib/log_table.hpp>
#include <loglib/log_value.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/parser_options.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stop_token.hpp>

#include <test_common/log_generator.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace loglib;
using namespace bench;
using test_common::GenerateRandomLogRecords;
using test_common::GenerateWideLogRecords;

namespace
{

// `JsonParser::ParseStreaming` adapter for `bench::RunStreamingBenchmark`.
// A free function rather than a `bench::ParserStreamFn` global to avoid
// `bugprone-throwing-static-initialization` (the `std::function` ctor is
// not `noexcept`).
void JsonStream(
    FileLineSource &source, LogParseSink &sink, const ParserOptions &options, internal::AdvancedParserOptions advanced
)
{
    JsonParser::ParseStreaming(source, sink, options, advanced);
}

} // namespace

// Synchronous-Parse coverage for `BufferingSink` (the sink behind the
// `loglib::ParseFile(parser, path)` free helper, used by tests and any
// non-GUI caller that wants a one-shot `LogData`). Small fixture because
// the synchronous path is not the GUI hot path -- the `[large]` and
// `[wide]` cases below cover that.
TEST_CASE("Parse and load JSON log (sync)", "[.][benchmark][json_parser][parse_sync]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    const TestStructuredLogFile testFile(GenerateRandomLogRecords(10'000), test_common::JsonLines());
    const JsonParser parser;
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    {
        const auto start = std::chrono::steady_clock::now();
        ParseResult warmup = ParseFile(parser, testFile.GetFilePath());
        const auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE(warmup.data.Lines().size() == testFile.RecordCount());
        REQUIRE(warmup.errors.empty());
        ReportThroughput("Parse 10'000 (sync) warm-up", elapsed, bytes, testFile.RecordCount());
    }

    RunTimedSamples(
        "Parse 10'000 JSON log entries (sync)", 5, {.bytes = bytes, .lines = testFile.RecordCount()}, [&]() {
            LogTable table;
            ParseResult result = ParseFile(parser, testFile.GetFilePath());
            REQUIRE(result.data.Lines().size() == testFile.RecordCount());
            REQUIRE(result.errors.empty());
            table.Update(std::move(result.data));
        }
    );
}

// Large-file streaming benchmark (1'000'000 lines, ~170 MB). End-to-end
// GUI flow: `LogTable::BeginStreaming` + a sink that calls
// `LogTable::AppendBatch` per `OnBatch`, with a `Type::Time` column so the
// streaming parser does real inline timestamp promotion.
TEST_CASE("Stream JSON log to LogTable (1'000'000 lines)", "[.][benchmark][json_parser][large]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    // Pinned seed + timestamps so the record bytes are identical across
    // runs and identical to `[logfmt_parser][large]`.
    const TestStructuredLogFile testFile(
        StreamedRecords{
            .count = 1'000'000, .seed = LARGE_FIXTURE_SEED, .timestamps = DeterministicBenchmarkTimestamps()
        },
        test_common::JsonLines()
    );
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    InitializeTimezoneData();

    auto configuration = MakeTimestampConfiguration();
    const TestLogConfiguration configFile;
    configFile.Write(*configuration);

    RunStreamingBenchmark(
        "Stream 1'000'000 JSON log entries to LogTable",
        configFile.GetFilePath(),
        testFile.GetFilePath(),
        configuration,
        JsonStream,
        testFile.RecordCount(),
        bytes,
        4
    );
}

// Wide-row streaming benchmark (200'000 lines, ~30 fields per line).
// Stresses the per-line field-iteration cost (`InsertSorted`,
// `ExtractFieldKey`, `ParseLine`, `IsKeyInAnyColumn`). Line count is lower
// than `[large]`'s 1'000'000 so total field operations stay comparable
// (~6x fields per line gives ~6 M total field ops at 200'000 lines).
TEST_CASE("Stream JSON log to LogTable (wide, 200'000 lines)", "[.][benchmark][json_parser][wide]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    // Pinned seed + timestamps so JSON and logfmt see byte-identical records.
    const TestStructuredLogFile testFile(
        GenerateWideLogRecords(200'000, /*columnCount=*/30, WIDE_FIXTURE_SEED, DeterministicBenchmarkTimestamps()),
        test_common::JsonLines()
    );
    // False positive inside MSVC's `<filesystem>` (`_BITMASK_OPS::operator|`).
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const size_t bytes = std::filesystem::file_size(testFile.GetFilePath());

    InitializeTimezoneData();

    auto configuration = MakeTimestampConfiguration();
    const TestLogConfiguration configFile;
    configFile.Write(*configuration);

    RunStreamingBenchmark(
        "Stream 200'000 wide JSON log entries to LogTable",
        configFile.GetFilePath(),
        testFile.GetFilePath(),
        configuration,
        JsonStream,
        testFile.RecordCount(),
        bytes,
        4
    );
}

// `LogLine::GetValue` micro-benchmark: slow path (key string -> `KeyIndex`
// -> `KeyId`) vs. fast path (cached `KeyId`). Pre-parses once so the
// timed loop measures lookup cost only.
TEST_CASE("LogLine::GetValue micro-benchmark", "[.][benchmark][log_line][get_value_micro]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    const TestStructuredLogFile testFile(GenerateRandomLogRecords(10'000), test_common::JsonLines());
    const JsonParser parser;

    ParseResult result = ParseFile(parser, testFile.GetFilePath());
    REQUIRE(result.errors.empty());
    const LogData &data = result.data;
    const std::vector<LogLine> &lines = data.Lines();
    REQUIRE(!lines.empty());

    const std::array<std::string, 5> kKeys = {"timestamp", "level", "message", "thread_id", "component"};
    std::array<KeyId, 5> keyIds{};
    for (size_t i = 0; i < kKeys.size(); ++i)
    {
        const KeyId id = data.Keys().Find(kKeys[i]);
        REQUIRE(id != INVALID_KEY_ID);
        keyIds[i] = id;
    }

    // `volatile` sink — `RunTimedSamples` has no `keep_memory()` analogue,
    // so without this the optimiser folds the whole walk away.
    volatile size_t hitsSink = 0;

    RunTimedSamples("LogLine::GetValue(string) — slow path", 11, [&]() {
        size_t hits = 0;
        for (const LogLine &line : lines)
        {
            for (const std::string &key : kKeys)
            {
                if (!std::holds_alternative<std::monostate>(line.GetValue(key)))
                {
                    ++hits;
                }
            }
        }
        hitsSink = hits;
    });

    RunTimedSamples("LogLine::GetValue(KeyId) — fast path", 11, [&]() {
        size_t hits = 0;
        for (const LogLine &line : lines)
        {
            for (const KeyId id : keyIds)
            {
                if (!std::holds_alternative<std::monostate>(line.GetValue(id)))
                {
                    ++hits;
                }
            }
        }
        hitsSink = hits;
    });

    // DictRef variant: pre-encode `level` so each `GetValue` resolves
    // through the registry; the gap vs the KeyId path is the lookup cost.
    {
        LogData &mutableData = result.data;
        const KeyId levelKey = mutableData.Keys().Find("level");
        REQUIRE(levelKey != INVALID_KEY_ID);

        EnumDictionaryRegistry registry;
        EnumDictionary &dict = registry.GetOrInsert(levelKey);
        std::vector<std::pair<size_t, EnumValueId>> rowDictRefs;
        rowDictRefs.reserve(mutableData.Lines().size());
        for (size_t i = 0; i < mutableData.Lines().size(); ++i)
        {
            const LogValue v = mutableData.Lines()[i].GetValue(levelKey);
            if (auto sv = AsStringView(v); sv.has_value())
            {
                const EnumValueId vid = dict.Insert(*sv);
                if (vid != INVALID_ENUM_VALUE_ID)
                {
                    rowDictRefs.emplace_back(i, vid);
                }
            }
        }

        for (auto &source : mutableData.Sources())
        {
            source->SetEnumDictionaries(&registry);
        }
        std::vector<LogLine> &mutLines = mutableData.Lines();
        for (auto &[rowIdx, vid] : rowDictRefs)
        {
            mutLines[rowIdx].SetOrReplaceEnumDictRef(levelKey, vid);
        }

        RunTimedSamples("LogLine::GetValue(KeyId) — DictRef path", 11, [&]() {
            size_t hits = 0;
            for (const LogLine &line : mutLines)
            {
                if (!std::holds_alternative<std::monostate>(line.GetValue(levelKey)))
                {
                    ++hits;
                }
            }
            hitsSink = hits;
        });
    }

    (void)hitsSink;
}

// Allocation footprint and `string_view` fast-path fraction. We don't
// override global `operator new` (interacts badly with Catch2 reporting
// and TBB workers); instead we count the observable structural cost of
// a parse: one heap-allocated vector per line, one owned-string
// allocation per `LogValue` holding a `std::string`, zero for values
// holding a `std::string_view`. The fast-path fraction has to stay > 0
// or the parser regressed to an all-owned-string regime.
TEST_CASE("Allocation footprint and string_view fast-path fraction", "[.][benchmark][json_parser][allocations]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    const TestStructuredLogFile testFile(GenerateRandomLogRecords(1'000), test_common::JsonLines());
    const JsonParser parser;

    ParseResult result = ParseFile(parser, testFile.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == testFile.RecordCount());

    // Phase 1 changed the per-field representation from a 48 B
    // `std::variant` to a 16 B `CompactLogValue` whose tags are the new
    // ground truth for "fast path vs slow path". `IsMmapSlice` /
    // `IsOwnedString` inspect the tag without materialising a `LogValue`
    // (which would always allocate a `std::string` for the slow path),
    // so the per-value cost of this benchmark stays representative of
    // the parse hot path.
    const size_t lineCount = result.data.Lines().size();
    size_t totalValues = 0;
    size_t mmapSliceValues = 0;
    size_t ownedStringValues = 0;
    size_t dictRefValues = 0;
    for (const LogLine &line : result.data.Lines())
    {
        for (size_t i = 0; i < result.data.Keys().Size(); ++i)
        {
            const auto id = static_cast<KeyId>(i);
            const bool mmap = line.IsMmapSlice(id);
            const bool owned = line.IsOwnedString(id);
            const bool dict = line.IsDictRef(id);
            if (mmap)
            {
                ++mmapSliceValues;
                ++totalValues;
            }
            else if (owned)
            {
                ++ownedStringValues;
                ++totalValues;
            }
            else if (dict)
            {
                ++dictRefValues;
                ++totalValues;
            }
            else if (!std::holds_alternative<std::monostate>(line.GetValue(id)))
            {
                ++totalValues;
            }
        }
    }

    const size_t totalStringValues = mmapSliceValues + ownedStringValues + dictRefValues;
    const double fastPathFraction =
        totalStringValues == 0 ? 0.0 : static_cast<double>(mmapSliceValues) / static_cast<double>(totalStringValues);
    // One vector heap allocation per line (the compact-pair vector), plus
    // *zero* per owned string (those are coalesced in the `LogFile`
    // arena post-stream — the per-batch arena is freed when Stage C ends).
    const size_t allocUpperBound = lineCount;

    WARN(
        "Allocation footprint over " << lineCount << " lines: " << totalValues << " values, " << mmapSliceValues
                                     << " MmapSlice (fast path), " << ownedStringValues << " OwnedString (slow path), "
                                     << dictRefValues << " DictRef (enum-encoded), fast-path fraction="
                                     << (fastPathFraction * 100.0) << "%, allocation upper bound=" << allocUpperBound
                                     << " (~" << (static_cast<double>(allocUpperBound) / static_cast<double>(lineCount))
                                     << "/line)"
    );

    REQUIRE(mmapSliceValues > 0);
}

// Enum auto-detection benchmark: a handful of distinct `level` values
// across many rows. Reports DictRef fraction and dictionary heap bytes.
TEST_CASE("Stream JSON log to LogTable (enum auto-detection)", "[.][benchmark][json_parser][enum]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    const TestStructuredLogFile testFile(GenerateRandomLogRecords(20'000), test_common::JsonLines());

    InitializeTimezoneData();
    auto configuration = MakeTimestampConfiguration();
    const TestLogConfiguration cfgFile;
    cfgFile.Write(*configuration);

    LogConfigurationManager configManager;
    configManager.Load(cfgFile.GetFilePath());
    LogTable table(LogData{}, std::move(configManager));

    auto sourceForTable = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *parseSource = sourceForTable.get();
    table.BeginStreaming(std::move(sourceForTable));

    StreamSink sink;
    sink.table = &table;

    ParserOptions opts;
    opts.configuration = std::move(configuration);

    JsonParser::ParseStreaming(*parseSource, sink, opts, internal::AdvancedParserOptions{});

    REQUIRE(table.RowCount() == testFile.RecordCount());

    const KeyId levelKey = table.Keys().Find("level");
    REQUIRE(levelKey != INVALID_KEY_ID);

    const auto &columns = table.Configuration().Configuration().columns;
    auto levelColumn = std::ranges::find_if(columns, [](const auto &c) {
        return std::ranges::find(c.keys, std::string("level")) != c.keys.end();
    });
    // The `level` column auto-promotes from Enumeration to the canonical
    // Level type once enough dictionary entries map to a `LogLevel`; the
    // benchmark checks that *either* terminal enum-like state was reached.
    const bool levelIsEnumeration =
        levelColumn != columns.end() && (levelColumn->type == LogConfiguration::Type::Enumeration ||
                                         levelColumn->type == LogConfiguration::Type::Level);

    size_t dictRefValues = 0;
    for (const LogLine &line : table.Data().Lines())
    {
        if (line.IsDictRef(levelKey))
        {
            ++dictRefValues;
        }
    }

    const StructuralBytesWithEnums bytes = ComputeStructuralBytesWithEnums(table);

    WARN(
        "Enum auto-detection over " << table.RowCount() << " lines: dict-ref values=" << dictRefValues << " ("
                                    << (100.0 * static_cast<double>(dictRefValues) /
                                        static_cast<double>(table.RowCount()))
                                    << "%), level column is enum or level=" << (levelIsEnumeration ? "yes" : "no")
                                    << ", dictionary heap bytes=" << bytes.enumDictionaries
                                    << ", lines+offsets+ownedStrings+keyIndex bytes=" << bytes.Total()
    );

    CHECK(levelIsEnumeration);
    CHECK(dictRefValues == table.RowCount());
}

// Cancellation-latency benchmark. Asks for a stop after the first batch
// arrives and times the gap between `request_stop()` and
// `OnFinished(cancelled=true)`. Validates the
// `ntokens * batchSizeBytes` upper bound on cancellation latency.
TEST_CASE("Cancellation latency", "[.][benchmark][json_parser][cancellation]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    // Pinned seed + timestamps for reproducible byte counts (the
    // measurement itself is independent of content variation).
    const TestStructuredLogFile testFile(
        StreamedRecords{
            .count = 1'000'000, .seed = LARGE_FIXTURE_SEED, .timestamps = DeterministicBenchmarkTimestamps()
        },
        test_common::JsonLines()
    );
    const JsonParser parser;

    struct LatencySink : LogParseSink
    {
        KeyIndex keys;
        loglib::StopSource stop;
        std::chrono::steady_clock::time_point requestedAt;
        std::chrono::steady_clock::time_point finishedAt;
        bool cancelled = false;
        size_t batches = 0;

        KeyIndex &Keys() override
        {
            return keys;
        }
        void OnStarted() override
        {
        }
        void OnBatch(StreamedBatch /*batch*/) override
        {
            ++batches;
            if (batches == 1 && !stop.stop_requested())
            {
                requestedAt = std::chrono::steady_clock::now();
                stop.request_stop();
            }
        }
        void OnFinished(bool wasCancelled) override
        {
            finishedAt = std::chrono::steady_clock::now();
            cancelled = wasCancelled;
        }
    };

    constexpr int ITERATIONS = 20;
    std::vector<double> latenciesUs;
    latenciesUs.reserve(ITERATIONS);

    for (int i = 0; i < ITERATIONS; ++i)
    {
        FileLineSource source(std::make_unique<LogFile>(testFile.GetFilePath()));
        LatencySink sink;
        ParserOptions opts;
        opts.stopToken = sink.stop.get_token();
        parser.ParseStreaming(source, sink, opts);
        REQUIRE(sink.cancelled);
        REQUIRE(sink.requestedAt.time_since_epoch().count() != 0);
        const auto latency = std::chrono::duration<double, std::micro>(sink.finishedAt - sink.requestedAt).count();
        latenciesUs.push_back(latency);
    }

    std::ranges::sort(latenciesUs);
    const double median = latenciesUs[latenciesUs.size() / 2];
    const double p95 = latenciesUs[(latenciesUs.size() * 95) / 100];
    const double maxLatency = latenciesUs.back();
    WARN(
        "Cancellation latency over " << ITERATIONS << " runs (us): median=" << median << ", p95=" << p95
                                     << ", max=" << maxLatency
    );
    REQUIRE(maxLatency < 5'000'000.0);
}
