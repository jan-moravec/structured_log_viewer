// JSON parser benchmarks for loglib. See CONTRIBUTING.md ## Benchmarking for the PR-process docs (regression
// gate, how to read the figures, how to run). The benchmark executable refuses to run in a Debug build.

#include "parse_only.hpp"

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"
#include "common.hpp"

#include <loglib/core/compact_log_value.hpp>
#include <loglib/core/enum_dictionary.hpp>
#include <loglib/core/file_line_source.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/core/log_value.hpp>
#include <loglib/core/stop_token.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/parse/log_parser.hpp>
#include <loglib/parse/parser_options.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/table/log_table.hpp>

#include <test_common/log_generator.hpp>

#include <benchmark/benchmark.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

using namespace loglib;
using test_common::GenerateRandomLogRecords;
using test_common::GenerateWideLogRecords;

namespace bench
{

namespace
{

// JsonParser::ParseStreaming adapter for RegisterStreaming. A free function rather than a ParserStreamFn global
// to avoid bugprone-throwing-static-initialization (the std::function ctor is not noexcept).
void JsonStream(FileLineSource &source, LogParseSink &sink, const ParserOptions &options)
{
    JsonParser{}.ParseStreaming(source, sink, options);
}

constexpr std::size_t SMALL_FILE_RECORDS = 10'000;
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const std::array<std::string, 5> LOOKUP_KEYS = {"timestamp", "level", "message", "thread_id", "component"};

// A parsed small file that the lookup benchmarks read.
struct ParsedInput
{
    std::unique_ptr<TestStructuredLogFile> file;
    ParseResult result;
    std::array<KeyId, 5> keyIds{};
    KeyId levelKey = INVALID_KEY_ID;
    // Only the DictRef input fills this: the registry the rewritten level values refer to.
    EnumDictionaryRegistry registry;
};

std::unique_ptr<ParsedInput> ParseFixture(std::size_t records, const char *fileName)
{
    auto input = std::make_unique<ParsedInput>();
    input->file = std::make_unique<TestStructuredLogFile>(
        GenerateRandomLogRecords(records), test_common::JsonLines(), test_common::RecordSchema{}, fileName
    );
    input->result = ParseFile(JsonParser{}, input->file->GetFilePath());
    if (!input->result.errors.empty() || input->result.data.Lines().size() != input->file->RecordCount())
    {
        throw std::runtime_error("the JSON fixture did not parse");
    }
    for (std::size_t i = 0; i < LOOKUP_KEYS.size(); ++i)
    {
        input->keyIds[i] = input->result.data.Keys().Find(LOOKUP_KEYS[i]);
        if (input->keyIds[i] == INVALID_KEY_ID)
        {
            throw std::runtime_error("the JSON fixture lacks a key");
        }
    }
    input->levelKey = input->result.data.Keys().Find("level");
    return input;
}

// Pre-encodes level as DictRef so each GetValue resolves through the registry; the gap to the KeyId path is the
// lookup cost.
std::unique_ptr<ParsedInput> ParseFixtureWithDictRefLevel()
{
    auto input = ParseFixture(SMALL_FILE_RECORDS, "bench_json_dictref.jsonl");
    LogData &data = input->result.data;
    EnumDictionary &dict = input->registry.GetOrInsert(input->levelKey);
    std::vector<std::pair<std::size_t, EnumValueId>> rowDictRefs;
    rowDictRefs.reserve(data.Lines().size());
    for (std::size_t i = 0; i < data.Lines().size(); ++i)
    {
        const LogValue v = data.Lines()[i].GetValue(input->levelKey);
        if (auto sv = AsStringView(v); sv.has_value())
        {
            const EnumValueId vid = dict.Insert(*sv);
            if (vid != INVALID_ENUM_VALUE_ID)
            {
                rowDictRefs.emplace_back(i, vid);
            }
        }
    }
    for (auto &source : data.Sources())
    {
        source->SetEnumDictionaries(&input->registry);
    }
    for (auto &[rowIdx, vid] : rowDictRefs)
    {
        *data.Lines()[rowIdx].FindCompactMutable(input->levelKey) = CompactLogValue::MakeDictRef(vid);
    }
    return input;
}

// Asks for a stop after the first batch and records when the parse reports it was cancelled.
struct LatencySink : LogParseSink
{
    KeyIndex keys;
    loglib::StopSource stop;
    std::chrono::steady_clock::time_point requestedAt;
    std::chrono::steady_clock::time_point finishedAt;
    bool cancelled = false;
    std::size_t batches = 0;

    KeyIndex &Keys() override
    {
        return keys;
    }
    void OnStarted() override
    {
    }
    void OnBatch(RowBatch /*batch*/) override
    {
        ++batches;
        if (batches == 1 && !stop.StopRequested())
        {
            requestedAt = std::chrono::steady_clock::now();
            stop.RequestStop();
        }
    }
    void OnFinished(bool wasCancelled) override
    {
        finishedAt = std::chrono::steady_clock::now();
        cancelled = wasCancelled;
    }
};

struct CancellationInput
{
    std::unique_ptr<TestStructuredLogFile> file;
};

struct SyncInput
{
    std::unique_ptr<TestStructuredLogFile> file;
    std::size_t bytes = 0;
};

struct EnumInput
{
    std::unique_ptr<TestStructuredLogFile> file;
    TestLogConfiguration configFile{"bench_json_enum_config.json"};
    std::shared_ptr<const LogConfiguration> configuration;
};

// One streaming parse into a fresh LogTable, kept alive so the counters can read the last one after the timed loop.
struct EnumRun
{
    std::unique_ptr<LogConfigurationManager> configManager;
    std::unique_ptr<loglib::LogTable> table;
};

// Walks every key of every line and counts the matches; fn(line) returns how many values it found.
template <typename Walk>
void RunLookupBenchmark(benchmark::State &state, std::span<const LogLine> lines, std::size_t keysPerLine, Walk walk)
{
    std::size_t hits = 0;
    for (const auto _ : state)
    {
        hits = 0;
        for (const LogLine &line : lines)
        {
            hits += walk(line);
        }
        benchmark::DoNotOptimize(hits);
    }
    if (hits == 0)
    {
        Fail(state, "no lookup found a value");
        return;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * lines.size() * keysPerLine));
}

// Rows that came from a StreamLineSource. Their strings sit in the source, behind its lock, and not in a mapped file.
struct StreamRows
{
    StreamLineSource source{std::filesystem::path("bench_stream_rows"), nullptr};
    std::vector<LogLine> lines;
    std::array<KeyId, 5> keyIds{};
};

std::unique_ptr<StreamRows> MakeStreamRows()
{
    auto rows = std::make_unique<StreamRows>();
    KeyIndex keys;
    for (std::size_t i = 0; i < LOOKUP_KEYS.size(); ++i)
    {
        rows->keyIds[i] = keys.GetOrInsert(LOOKUP_KEYS[i]);
    }
    rows->lines.reserve(SMALL_FILE_RECORDS);
    for (std::size_t row = 0; row < SMALL_FILE_RECORDS; ++row)
    {
        // The keys are timestamp, level, message, thread_id and component. All but thread_id are strings.
        const std::array<std::string, 5> texts = {
            "2026-01-01T00:00:00.000Z",
            "info",
            "request handled for user " + std::to_string(row),
            "",
            "api",
        };
        std::string arena;
        std::vector<std::pair<KeyId, CompactLogValue>> values;
        for (std::size_t i = 0; i < texts.size(); ++i)
        {
            if (i == 3)
            {
                values.emplace_back(rows->keyIds[i], CompactLogValue::MakeInt64(static_cast<std::int64_t>(row % 16)));
                continue;
            }
            values.emplace_back(
                rows->keyIds[i],
                CompactLogValue::MakeOwnedString(arena.size(), static_cast<std::uint32_t>(texts[i].size()))
            );
            arena += texts[i];
        }
        const std::size_t lineId = rows->source.AppendLine("raw line", std::move(arena));
        rows->lines.emplace_back(std::move(values), rows->source, lineId);
    }
    return rows;
}

void RegisterLookupBenchmarks()
{
    const auto plainInput =
        MakeLazy<ParsedInput>([] { return ParseFixture(SMALL_FILE_RECORDS, "bench_json_lookup.jsonl"); });
    const auto dictRefInput = MakeLazy<ParsedInput>(ParseFixtureWithDictRefLevel);

    // LogLine::GetValue micro-benchmarks: slow path (key string -> KeyIndex -> KeyId) vs. fast path (cached
    // KeyId) vs. the enum-encoded path. The file is parsed once outside the timed part, so the timed loop measures
    // lookup cost only. One iteration looks up every key of every line.
    RegisterSmall("LogLine::GetValue(string) - slow path", [plainInput](benchmark::State &state) {
        const LogData &data = plainInput->Get().result.data;
        RunLookupBenchmark(state, data.Lines(), LOOKUP_KEYS.size(), [&data](const LogLine &line) {
            std::size_t found = 0;
            for (const std::string &key : LOOKUP_KEYS)
            {
                found += !std::holds_alternative<std::monostate>(line.GetValue(data.Keys(), key));
            }
            return found;
        });
    });

    RegisterSmall("LogLine::GetValue(KeyId) - fast path", [plainInput](benchmark::State &state) {
        const ParsedInput &input = plainInput->Get();
        RunLookupBenchmark(state, input.result.data.Lines(), input.keyIds.size(), [&input](const LogLine &line) {
            std::size_t found = 0;
            for (const KeyId id : input.keyIds)
            {
                found += !std::holds_alternative<std::monostate>(line.GetValue(id));
            }
            return found;
        });
    });

    // The same lookups on rows from a live stream, whose strings resolve through the source's lock.
    const auto streamInput = MakeLazy<StreamRows>(MakeStreamRows);
    RegisterSmall("LogLine::GetValue(KeyId) - stream rows", [streamInput](benchmark::State &state) {
        const StreamRows &input = streamInput->Get();
        RunLookupBenchmark(state, input.lines, input.keyIds.size(), [&input](const LogLine &line) {
            std::size_t found = 0;
            for (const KeyId id : input.keyIds)
            {
                found += !std::holds_alternative<std::monostate>(line.GetValue(id));
            }
            return found;
        });
    });

    RegisterSmall("LogLine::GetValue(KeyId) - DictRef path", [dictRefInput](benchmark::State &state) {
        const ParsedInput &input = dictRefInput->Get();
        RunLookupBenchmark(state, input.result.data.Lines(), 1, [&input](const LogLine &line) {
            return static_cast<std::size_t>(!std::holds_alternative<std::monostate>(line.GetValue(input.levelKey)));
        });
    });
}

// Allocation footprint and string_view fast-path fraction. We don't override global operator new (it interacts
// badly with TBB workers); instead we count the observable structural cost of a parse: one heap-allocated vector per
// line, one owned-string allocation per value holding a std::string, zero for values holding a std::string_view.
// The compact slot's tag is inspected without materialising a LogValue (which would always allocate a
// std::string for the slow path). The fast-path fraction has to stay above zero or the parser regressed to an
// all-owned-string regime. The timed part is the classification walk.
void RegisterAllocationBenchmark()
{
    const auto input = MakeLazy<ParsedInput>([] { return ParseFixture(1'000, "bench_json_alloc.jsonl"); });
    RegisterSmall("Allocation footprint and string_view fast-path fraction", [input](benchmark::State &state) {
        const LogData &data = input->Get().result.data;
        std::size_t totalValues = 0;
        std::size_t mmapSliceValues = 0;
        std::size_t ownedStringValues = 0;
        std::size_t dictRefValues = 0;
        for (const auto _ : state)
        {
            totalValues = mmapSliceValues = ownedStringValues = dictRefValues = 0;
            for (const LogLine &line : data.Lines())
            {
                for (std::size_t i = 0; i < data.Keys().Size(); ++i)
                {
                    const auto id = static_cast<KeyId>(i);
                    const CompactLogValue *compact = line.FindCompact(id);
                    if (compact != nullptr && compact->tag == CompactTag::MmapSlice)
                    {
                        ++mmapSliceValues;
                        ++totalValues;
                    }
                    else if (compact != nullptr && compact->tag == CompactTag::OwnedString)
                    {
                        ++ownedStringValues;
                        ++totalValues;
                    }
                    else if (line.IsDictRef(id))
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
            benchmark::DoNotOptimize(totalValues);
        }
        if (mmapSliceValues == 0)
        {
            Fail(state, "no value took the string_view fast path");
            return;
        }
        const std::size_t totalStringValues = mmapSliceValues + ownedStringValues + dictRefValues;
        state.counters["lines"] = static_cast<double>(data.Lines().size());
        state.counters["values"] = static_cast<double>(totalValues);
        state.counters["MmapSlice"] = static_cast<double>(mmapSliceValues);
        state.counters["OwnedString"] = static_cast<double>(ownedStringValues);
        state.counters["DictRef"] = static_cast<double>(dictRefValues);
        state.counters["fast_path_fraction_pct"] =
            100.0 * static_cast<double>(mmapSliceValues) / static_cast<double>(totalStringValues);
        // One vector heap allocation per line (the compact-pair vector), plus zero per owned string: those are
        // coalesced in the FileLineSource arena after the stream.
        state.counters["allocation_upper_bound"] = static_cast<double>(data.Lines().size());
    });
}

// Enum auto-detection: a handful of distinct level values across many rows. The timed part is the streaming parse
// into a fresh table; the counters report the DictRef fraction and the dictionary heap bytes.
void RegisterEnumBenchmark()
{
    const auto input = MakeLazy<EnumInput>([] {
        auto created = std::make_unique<EnumInput>();
        created->file = std::make_unique<TestStructuredLogFile>(
            GenerateRandomLogRecords(20'000),
            test_common::JsonLines(),
            test_common::RecordSchema{},
            "bench_json_enum.jsonl"
        );
        InitializeTimezoneData();
        created->configuration = MakeTimestampConfiguration();
        created->configFile.Write(*created->configuration);
        return created;
    });
    RegisterSmall("Enum auto-detection over 20000 lines", [input](benchmark::State &state) {
        EnumInput &fixture = input->Get();
        EnumRun run;
        for (const auto _ : state)
        {
            run.table.reset();
            run.configManager = std::make_unique<LogConfigurationManager>();
            run.configManager->Load(fixture.configFile.GetFilePath());
            run.table = std::make_unique<loglib::LogTable>(run.configManager->Configuration());
            run.table->SetDisplayTimeZone(ProcessDefaultTimeZone());
            run.table->SetDefaultSourceTimeZone(ProcessDefaultTimeZone());

            auto sourceForTable = std::make_unique<FileLineSource>(fixture.file->GetFilePath());
            FileLineSource *parseSource = sourceForTable.get();
            run.table->BeginStreaming(std::move(sourceForTable));

            StreamSink sink;
            sink.table = run.table.get();
            ParserOptions opts;
            opts.timeColumns = TimeColumnsOf(*fixture.configuration);
            opts.defaultSourceTimeZone = run.table->DefaultSourceTimeZone();
            JsonParser{}.ParseStreaming(*parseSource, sink, opts);
        }

        const loglib::LogTable &table = *run.table;
        const KeyId levelKey = table.Keys().Find("level");
        const auto &columns = table.Configuration().Configuration().columns;
        const auto levelColumn = std::ranges::find_if(columns, [](const auto &c) {
            return std::ranges::find(c.keys, std::string("level")) != c.keys.end();
        });
        // The level column auto-promotes from Enumeration to the canonical Level type once enough dictionary
        // entries map to a LogLevel; either terminal enum-like state is accepted.
        const bool levelIsEnumeration = levelColumn != columns.end() && (levelColumn->type == ColumnType::Enumeration ||
                                                                         levelColumn->type == ColumnType::Level);
        std::size_t dictRefValues = 0;
        if (levelKey != INVALID_KEY_ID)
        {
            for (const LogLine &line : table.Data().Lines())
            {
                dictRefValues += line.IsDictRef(levelKey) ? 1 : 0;
            }
        }
        if (table.RowCount() != fixture.file->RecordCount() || !levelIsEnumeration || dictRefValues != table.RowCount())
        {
            Fail(state, "the level column was not enum-encoded on every row");
            return;
        }
        const StructuralBytesWithEnums bytes = ComputeStructuralBytesWithEnums(table);
        state.counters["dict_ref_values"] = static_cast<double>(dictRefValues);
        state.counters["dict_ref_pct"] =
            100.0 * static_cast<double>(dictRefValues) / static_cast<double>(table.RowCount());
        state.counters["dictionary_heap_bytes"] = static_cast<double>(bytes.enumDictionaries);
        state.counters["structural_bytes"] = static_cast<double>(bytes.Total());
        state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * table.RowCount()));
    });
}

// Synchronous-Parse coverage for BufferingSink (the sink behind the loglib::ParseFile(parser, path) free helper,
// used by tests and any non-GUI caller that wants a one-shot LogData). Small fixture because the synchronous path is
// not the GUI hot path -- the large and wide streams cover that.
void RegisterSyncBenchmark()
{
    const auto input = MakeLazy<SyncInput>([] {
        auto created = std::make_unique<SyncInput>();
        created->file = std::make_unique<TestStructuredLogFile>(
            GenerateRandomLogRecords(SMALL_FILE_RECORDS),
            test_common::JsonLines(),
            test_common::RecordSchema{},
            "bench_json_sync.jsonl"
        );
        created->bytes = std::filesystem::file_size(created->file->GetFilePath());
        return created;
    });
    RegisterSmall("Parse 10'000 JSON log entries (sync)", [input](benchmark::State &state) {
        const SyncInput &fixture = input->Get();
        const JsonParser parser;
        std::size_t lines = 0;
        std::size_t errors = 0;
        for (const auto _ : state)
        {
            ParseResult result = ParseFile(parser, fixture.file->GetFilePath());
            lines = result.data.Lines().size();
            errors = result.errors.size();
            benchmark::DoNotOptimize(result);
        }
        if (lines != fixture.file->RecordCount() || errors != 0)
        {
            Fail(state, "the synchronous parse returned the wrong rows");
            return;
        }
        const auto iterations = static_cast<std::int64_t>(state.iterations());
        state.SetBytesProcessed(iterations * static_cast<std::int64_t>(fixture.bytes));
        state.SetItemsProcessed(iterations * static_cast<std::int64_t>(fixture.file->RecordCount()));
    });
}

// Asks for a stop after the first batch arrives and times the gap between RequestStop() and
// OnFinished(cancelled=true), which validates the ntokens * batchSizeBytes upper bound on latency. The gap is the
// iteration time; a repetition is 20 cancelled parses. Pinned seed and timestamps give reproducible byte counts (the
// gap itself does not depend on content).
void RegisterCancellationBenchmark()
{
    RegisterLarge<CancellationInput>(
        "Cancellation latency over 20 runs (us)",
        [] {
            auto input = std::make_unique<CancellationInput>();
            input->file = std::make_unique<TestStructuredLogFile>(
                StreamedRecords{
                    .count = 1'000'000,
                    .seed = LARGE_FIXTURE_SEED,
                    .timestamps = DeterministicBenchmarkTimestamps(),
                },
                test_common::JsonLines(),
                test_common::RecordSchema{},
                "bench_json_cancel.jsonl"
            );
            return input;
        },
        [](CancellationInput &input, benchmark::State &state, bool first) {
            const JsonParser parser;
            // Returns the gap in seconds, or a negative number when the parse was not cancelled.
            const auto cancelOnce = [&] {
                FileLineSource source(input.file->GetFilePath());
                LatencySink sink;
                ParserOptions opts;
                opts.stopToken = sink.stop.GetToken();
                parser.ParseStreaming(source, sink, opts);
                if (!sink.cancelled || sink.requestedAt.time_since_epoch().count() == 0)
                {
                    return -1.0;
                }
                return std::chrono::duration<double>(sink.finishedAt - sink.requestedAt).count();
            };
            // The first runs of a process are slower than the steady state; the warm-up is one untimed repetition.
            for (int i = 0; first && i < 20; ++i)
            {
                cancelOnce();
            }
            for (const auto _ : state)
            {
                const double gap = cancelOnce();
                if (gap < 0.0)
                {
                    Fail(state, "the parse was not cancelled");
                    return;
                }
                state.SetIterationTime(gap);
            }
        },
        LargeOptions{.iterations = 20, .manualTime = true, .unit = benchmark::kMicrosecond}
    );
}

} // namespace

void RegisterJsonBenchmarks()
{
    RegisterSyncBenchmark();

    // Large-file streaming benchmark (1'000'000 lines, ~170 MB). End-to-end GUI flow: LogTable::BeginStreaming plus
    // a sink that calls LogTable::AppendBatch per OnBatch, with a Type::Time column so the streaming parser
    // does real inline timestamp promotion. Pinned seed and timestamps keep the record bytes identical across runs
    // and to the logfmt benchmark.
    // It is registered a second time at the smoke size.
    for (const Tier &tier : {FULL_TIER, SMOKE_TIER})
    {
        RegisterStreaming(
            TierName("Stream " + CountText(tier.rows) + " JSON log entries to LogTable", tier),
            [rows = tier.rows] {
                return StreamingFixture{
                    .logFile = std::make_unique<TestStructuredLogFile>(
                        StreamedRecords{
                            .count = rows,
                            .seed = LARGE_FIXTURE_SEED,
                            .timestamps = DeterministicBenchmarkTimestamps(),
                        },
                        test_common::JsonLines()
                    ),
                    .parser = JsonStream,
                };
            },
            /*parseLoopOnly=*/false,
            MEDIUM_LARGE_ITERATIONS,
            tier.largeRepetitions
        );
    }

    // Wide-row streaming benchmark (200'000 lines, ~30 fields per line). Stresses the per-line field-iteration cost
    // (InsertSorted, ExtractFieldKey, ParseLine, IsKeyInAnyColumn). The line count is lower than the large
    // one so total field operations stay comparable.
    RegisterStreaming(
        "Stream 200'000 wide JSON log entries to LogTable",
        [] {
            return StreamingFixture{
                .logFile = std::make_unique<TestStructuredLogFile>(
                    GenerateWideLogRecords(
                        200'000, /*columnCount=*/30, WIDE_FIXTURE_SEED, DeterministicBenchmarkTimestamps()
                    ),
                    test_common::JsonLines()
                ),
                .parser = JsonStream,
            };
        },
        /*parseLoopOnly=*/false,
        FAST_LARGE_ITERATIONS
    );

    RegisterLookupBenchmarks();
    RegisterAllocationBenchmark();
    RegisterEnumBenchmark();
    RegisterCancellationBenchmark();
}

} // namespace bench
