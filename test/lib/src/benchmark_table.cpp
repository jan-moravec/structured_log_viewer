// LogTable benchmarks on a table with one column of each common type: cell formatting, sorting on a Time column and a
// free-text column, regex filters on the message, and a stream that keeps only the newest rows. The table is built once
// outside the timed part from a fixed seed, so every run reads the same rows. See CONTRIBUTING.md ## Benchmarking.

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"
#include "common.hpp"
#include "table_fixture.hpp"

#include <loglib/core/compact_log_value.hpp>
#include <loglib/core/file_line_source.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/core/log_value.hpp>
#include <loglib/core/row_batch.hpp>
#include <loglib/core/stream_line_source.hpp>
#include <loglib/query/leaf_rule_compile.hpp>
#include <loglib/query/log_compare.hpp>
#include <loglib/query/log_filter.hpp>
#include <loglib/schema/column_schema.hpp>
#include <loglib/schema/filter_expression.hpp>
#include <loglib/schema/log_configuration.hpp>
#include <loglib/table/log_table.hpp>
#include <loglib/time/time_zone_context.hpp>

#include <benchmark/benchmark.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numeric>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace loglib;

namespace bench
{

namespace
{

constexpr std::size_t TIME_COLUMN = 0;
constexpr std::size_t MESSAGE_COLUMN = 1;
constexpr std::size_t COUNT_COLUMN = 2;
constexpr std::size_t RATIO_COLUMN = 3;
constexpr std::size_t REGION_COLUMN = 4;
// Only in the table that is built with text columns.
constexpr std::size_t PATH_COLUMN = 5;
constexpr std::size_t SPACED_COLUMN = 6;

constexpr std::size_t BATCH_ROWS = 50'000;

// One row in ten has this message, so the regex filters accept about 10 % of the rows.
constexpr std::string_view TIMEOUT_PREFIX = "request failed: timeout after ";
constexpr std::string_view CASE_SENSITIVE_PATTERN = "timeout after \\d+ ms";
constexpr std::string_view CASE_INSENSITIVE_PATTERN = "(?i)TIMEOUT after \\d+ MS";

struct MixedTable
{
    std::unique_ptr<TestLogFile> file;
    std::unique_ptr<LogTable> table;
    std::size_t rows = 0;
    std::size_t timeoutRows = 0;
};

Column MakeColumn(const std::string &name, ColumnType type, const std::string &printFormat)
{
    Column column;
    column.header = name;
    column.keys = {name};
    column.type = type;
    column.printFormat = printFormat;
    return column;
}

// A table of rows rows with a Time, a String, an Integer, a Floating and an Enumeration column. The Time column holds
// shuffled values, so sorting it has work to do. With textColumns it also has a path column, whose cells share a long
// prefix, and a spaced column, which holds the message with every space doubled in half of the rows.
std::unique_ptr<MixedTable> BuildMixedTable(std::size_t rows, const char *fileName, bool textColumns = false)
{
    InitializeTimezoneData();
    auto result = std::make_unique<MixedTable>();
    result->rows = rows;
    result->file = std::make_unique<TestLogFile>(fileName);
    result->file->Write("");

    LogConfiguration configuration;
    Column time = MakeColumn("time", ColumnType::Time, "%F %H:%M:%S");
    time.parseFormats = {"%FT%T"};
    configuration.columns.push_back(std::move(time));
    configuration.columns.push_back(MakeColumn("message", ColumnType::String, "{}"));
    configuration.columns.push_back(MakeColumn("count", ColumnType::Integer, "{}"));
    configuration.columns.push_back(MakeColumn("ratio", ColumnType::Floating, "{:.3f}"));
    configuration.columns.push_back(MakeColumn("region", ColumnType::Enumeration, "{}"));
    if (textColumns)
    {
        configuration.columns.push_back(MakeColumn("path", ColumnType::String, "{}"));
        configuration.columns.push_back(MakeColumn("spaced", ColumnType::String, "{}"));
    }

    // A configuration that went through the manager is the one the application has, with its columns normalized.
    const TestLogConfiguration configurationFile("benchmark_table_configuration.json");
    configurationFile.Write(configuration);
    LogConfigurationManager manager;
    manager.Load(configurationFile.GetFilePath());
    result->table = std::make_unique<LogTable>(manager.Configuration());
    LogTable &table = *result->table;
    table.SetDisplayTimeZone(ProcessDefaultTimeZone());
    table.SetDefaultSourceTimeZone(ProcessDefaultTimeZone());
    auto source = std::make_unique<FileLineSource>(result->file->GetFilePath());
    FileLineSource &sourceRef = *source;
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    std::vector<std::string> names = {"time", "message", "count", "ratio", "region"};
    if (textColumns)
    {
        names.emplace_back("path");
        names.emplace_back("spaced");
    }
    std::vector<KeyId> ids;
    ids.reserve(names.size());
    for (const std::string &name : names)
    {
        ids.push_back(keys.GetOrInsert(name));
    }

    const std::vector<std::string> otherMessages = {
        "request handled for user",
        "cache miss on key",
        "background sweep complete",
        "auth token refresh ok",
        "shutdown signal received",
        "connection pool resized",
    };
    const std::vector<std::string> regions = {"north", "south", "east", "west"};
    const TimeStamp base{std::chrono::seconds{1767225600}};

    // NOLINTNEXTLINE(cert-msc32-c, cert-msc51-cpp, bugprone-random-generator-seed)
    std::mt19937 rng{0xC0FFEE3U};
    std::uniform_int_distribution<std::int64_t> timeOffset{0, std::int64_t{86'400} * 1'000'000};
    for (std::size_t first = 0; first < rows; first += BATCH_ROWS)
    {
        const std::size_t batchRows = std::min(BATCH_ROWS, rows - first);
        RowBatch batch;
        batch.firstLineNumber = first + 1;
        batch.lines.reserve(batchRows);
        for (std::size_t i = 0; i < batchRows; ++i)
        {
            const std::uint32_t pick = rng();
            std::string message;
            if (pick % 10 == 0)
            {
                message = std::string(TIMEOUT_PREFIX) + std::to_string(100 + (rng() % 900)) + " ms";
                ++result->timeoutRows;
            }
            else
            {
                message = otherMessages[(pick / 10) % otherMessages.size()] + " id=" + std::to_string(rng() % 100'000);
            }
            std::string spaced;
            if (textColumns)
            {
                // The doubled spaces come from the row's draw, so the random sequence stays the one of the table
                // without text columns.
                for (const char ch : message)
                {
                    spaced.push_back(ch);
                    if (ch == ' ' && (pick & 1U) != 0U)
                    {
                        spaced.push_back(' ');
                    }
                }
            }
            std::vector<std::pair<KeyId, LogValue>> values;
            values.reserve(names.size());
            values.emplace_back(ids[TIME_COLUMN], base + std::chrono::microseconds{timeOffset(rng)});
            values.emplace_back(ids[MESSAGE_COLUMN], std::move(message));
            values.emplace_back(ids[COUNT_COLUMN], static_cast<std::int64_t>(rng() % 1'000'000));
            values.emplace_back(ids[RATIO_COLUMN], static_cast<double>(rng() % 1'000'000) / 1000.0);
            values.emplace_back(ids[REGION_COLUMN], regions[rng() % regions.size()]);
            if (textColumns)
            {
                values.emplace_back(
                    ids[PATH_COLUMN],
                    "/var/log/application/services/ingest/workers/shard-" + std::to_string(pick % 100'000)
                );
                values.emplace_back(ids[SPACED_COLUMN], std::move(spaced));
            }
            // A row keeps its values sorted by key id, which the key index does not promise to hand out in order.
            std::ranges::sort(values, [](const auto &lhs, const auto &rhs) { return lhs.first < rhs.first; });
            batch.lines.emplace_back(std::move(values), sourceRef, 0);
        }
        if (first == 0)
        {
            for (const std::string &name : names)
            {
                batch.newKeys.emplace_back(name);
            }
        }
        AppendFixtureBatch(table, std::move(batch));
    }
    if (table.RowCount() != rows || result->timeoutRows == 0)
    {
        throw std::runtime_error("the mixed table was not built");
    }
    return result;
}

using SharedMixed = std::shared_ptr<LazyInput<MixedTable>>;

SharedMixed MakeSharedMixed(const Tier &tier)
{
    return MakeLazy<MixedTable>([rows = tier.rows, smoke = tier.smoke] {
        return BuildMixedTable(rows, smoke ? "benchmark_table_smoke.json" : "benchmark_table.json");
    });
}

// The table with the path and spaced columns, for the sorts and filters that need them.
SharedMixed MakeSharedTextMixed(const Tier &tier)
{
    return MakeLazy<MixedTable>([rows = tier.rows] {
        return BuildMixedTable(rows, "benchmark_table_text.json", true);
    });
}

struct MixedInput
{
    SharedMixed table;
};

// Formats every cell of one column. The check is that every cell has text and the first one looks like its type.
void RegisterFormatting(
    const Tier &tier,
    const SharedMixed &table,
    const char *label,
    std::size_t column,
    std::string firstPrefix,
    std::size_t rows,
    int iterations
)
{
    RegisterLarge<MixedInput>(
        TierName(std::string("LogTable cell formatting, ") + label + " column, " + CountText(rows) + " rows", tier),
        [table] { return std::make_unique<MixedInput>(MixedInput{.table = table}); },
        [column, firstPrefix = std::move(firstPrefix), rows](MixedInput &input, benchmark::State &state, bool first) {
            const MixedTable &fixture = input.table->Get();
            const LogTable &logTable = *fixture.table;
            const auto formatAll = [&] {
                std::size_t characters = 0;
                for (std::size_t row = 0; row < rows; ++row)
                {
                    characters += logTable.GetFormattedValue(row, column).size();
                }
                return characters;
            };
            std::size_t characters = first ? formatAll() : 0;
            for (const auto _ : state)
            {
                characters = formatAll();
                benchmark::DoNotOptimize(characters);
            }
            if (characters < rows || !logTable.GetFormattedValue(0, column).starts_with(firstPrefix))
            {
                Fail(
                    state,
                    "a cell did not format as expected: " + std::to_string(characters) + " characters, first cell '" +
                        logTable.GetFormattedValue(0, column) + "'"
                );
                return;
            }
            state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * rows));
        },
        LargeOptions{.repetitions = tier.largeRepetitions, .iterations = iterations}
    );
}

// SortPermutationByColumn over all rows. After the timed part the permutation is checked against CompareRows.
void RegisterSort(
    const Tier &tier, const SharedMixed &table, const char *label, std::size_t column, bool ascending, std::size_t rows
)
{
    RegisterSmall(
        TierName(
            std::string("SortPermutationByColumn ") + label + (ascending ? " ascending" : " descending") + " over " +
                CountText(rows) + " rows",
            tier
        ),
        [table, column, ascending, rows](benchmark::State &state) {
            const MixedTable &fixture = table->Get();
            const LogTable &logTable = *fixture.table;
            std::vector<std::size_t> logRows(rows);
            std::iota(logRows.begin(), logRows.end(), std::size_t{0});
            std::vector<std::size_t> permutation;
            for (const auto _ : state)
            {
                permutation =
                    SortPermutationByColumn(logTable, std::span<const std::size_t>{logRows}, column, ascending);
                benchmark::DoNotOptimize(permutation);
            }
            bool ordered = permutation.size() == rows;
            for (std::size_t i = 1; ordered && i < permutation.size(); ++i)
            {
                const int order = CompareRows(logTable, logRows[permutation[i - 1]], logRows[permutation[i]], column);
                ordered = ascending ? order <= 0 : order >= 0;
            }
            if (!ordered)
            {
                Fail(state, "the permutation is not sorted");
                return;
            }
            state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * rows));
        },
        benchmark::kMillisecond,
        tier
    );
}

// A filter whose only leaf is a regular expression on the message. The accepted rows must be exactly the timeout rows.
void RegisterRegexFilter(const Tier &tier, const SharedMixed &table, const char *label, std::string_view pattern)
{
    RegisterSmall(
        TierName(std::string("Regex filter, ") + label + ", over " + CountText(tier.rows) + " rows", tier),
        [table, pattern = std::string(pattern)](benchmark::State &state) {
            const MixedTable &fixture = table->Get();
            const LogTable &logTable = *fixture.table;
            FilterExpression expression;
            LeafRule rule;
            rule.type = LeafRule::Type::String;
            rule.columnKeys = {"message"};
            rule.matchType = LeafRule::Match::RegularExpression;
            rule.filterString = pattern;
            expression.node = FilterExpression::Leaf{rule};
            const CompiledFilterExpression compiled =
                CompileExpression(expression, logTable.Configuration().Configuration().columns, &logTable);
            std::size_t accepted = 0;
            for (const auto _ : state)
            {
                std::vector<std::size_t> rows = FilterAcceptedRows(logTable, compiled);
                accepted = rows.size();
                benchmark::DoNotOptimize(rows);
            }
            if (accepted != fixture.timeoutRows)
            {
                Fail(
                    state,
                    "the filter accepted " + std::to_string(accepted) + " rows, expected " +
                        std::to_string(fixture.timeoutRows)
                );
                return;
            }
            state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * fixture.rows));
            state.counters["accepted"] = static_cast<double>(accepted);
        },
        benchmark::kMillisecond,
        tier
    );
}

// A filter with one leaf, compiled and run over the whole table. The accepted rows must be as many as expected counts
// with plain reads, outside the timed part.
template <typename Expected>
void RegisterLeafFilter(
    const Tier &tier, const SharedMixed &table, const std::string &label, const LeafRule &rule, const Expected &expected
)
{
    RegisterSmall(
        TierName(label + " over " + CountText(tier.rows) + " rows", tier),
        [table, rule, expected](benchmark::State &state) {
            const MixedTable &fixture = table->Get();
            const LogTable &logTable = *fixture.table;
            FilterExpression expression;
            expression.node = FilterExpression::Leaf{rule};
            const CompiledFilterExpression compiled =
                CompileExpression(expression, logTable.Configuration().Configuration().columns, &logTable);
            std::size_t accepted = 0;
            for (const auto _ : state)
            {
                std::vector<std::size_t> rows = FilterAcceptedRows(logTable, compiled);
                accepted = rows.size();
                benchmark::DoNotOptimize(rows);
            }
            const std::size_t wanted = expected(fixture);
            if (accepted != wanted || accepted == 0 || accepted >= fixture.rows)
            {
                Fail(
                    state,
                    "the filter accepted " + std::to_string(accepted) + " rows, expected " + std::to_string(wanted)
                );
                return;
            }
            state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * fixture.rows));
            state.counters["accepted"] = static_cast<double>(accepted);
        },
        benchmark::kMillisecond,
        tier
    );
}

// The first count rows of a sort by one column, which is what a query with a limit needs. The rows must be in order
// and the first one must be the row a plain scan finds.
void RegisterTopRows(
    const Tier &tier, const SharedMixed &table, const char *label, std::size_t column, std::size_t count
)
{
    RegisterSmall(
        TierName(
            "Sorted top " + CountText(count) + " rows by " + label + " over " + CountText(tier.rows) + " rows", tier
        ),
        [table, column, count](benchmark::State &state) {
            const MixedTable &fixture = table->Get();
            const LogTable &logTable = *fixture.table;
            std::vector<std::size_t> logRows(fixture.rows);
            std::iota(logRows.begin(), logRows.end(), std::size_t{0});
            std::vector<std::size_t> top;
            for (const auto _ : state)
            {
                top = SortPermutationWindowByColumn(
                    logTable, std::span<const std::size_t>{logRows}, column, true, 0, count
                );
                benchmark::DoNotOptimize(top);
            }
            std::size_t smallest = 0;
            for (std::size_t row = 1; row < fixture.rows; ++row)
            {
                if (CompareRows(logTable, row, smallest, column) < 0)
                {
                    smallest = row;
                }
            }
            bool ordered = top.size() == count && top.front() == smallest;
            for (std::size_t i = 1; ordered && i < top.size(); ++i)
            {
                ordered = CompareRows(logTable, top[i - 1], top[i], column) <= 0;
            }
            if (!ordered)
            {
                Fail(state, "the top rows are not the smallest rows in order");
                return;
            }
            state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * fixture.rows));
        },
        benchmark::kMillisecond,
        tier
    );
}

// A batch of count rows with one Integer cell, backed by lines appended to the stream source. The cell holds the line
// number, so the surviving rows can be told after eviction.
RowBatch MakeStreamBatch(
    StreamLineSource &source, KeyId valueKey, std::size_t firstLineId, std::size_t count, bool declareKey
)
{
    RowBatch batch;
    batch.firstLineNumber = firstLineId;
    if (declareKey)
    {
        batch.newKeys.emplace_back("value");
    }
    batch.lines.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const std::size_t lineId = source.AppendLine("row " + std::to_string(firstLineId + i), std::string{});
        std::vector<std::pair<KeyId, CompactLogValue>> values;
        values.emplace_back(valueKey, CompactLogValue::MakeInt64(static_cast<std::int64_t>(lineId)));
        batch.lines.emplace_back(std::move(values), source, lineId);
    }
    return batch;
}

struct NoInput
{
};

// A stream of rows into a LogTable that keeps the newest tenth, evicting after every batch of 1000, as a live tail with
// a retention limit does. Building the batch is the parser's work and is not timed; the figure is AppendBatch and
// EvictPrefixRows.
void RegisterRetention(const Tier &tier)
{
    constexpr std::size_t BATCH = 1000;
    const std::size_t keep = tier.rows / 10;
    RegisterLarge<NoInput>(
        TierName(
            "Stream " + CountText(tier.rows) + " rows into a LogTable that keeps the newest " + CountText(keep), tier
        ),
        [] { return std::make_unique<NoInput>(); },
        [rows = tier.rows, keep](NoInput & /*input*/, benchmark::State &state, bool first) {
            // Returns the time of the table calls in seconds, or a negative number when the surviving rows are wrong.
            const auto streamOnce = [&] {
                auto owned = std::make_unique<StreamLineSource>(std::filesystem::path("retention"), nullptr);
                StreamLineSource &source = *owned;
                LogTable table;
                table.BeginStreaming(std::move(owned));
                const KeyId valueKey = table.Keys().GetOrInsert(std::string_view("value"));
                std::chrono::steady_clock::duration timed{};
                for (std::size_t firstLine = 1; firstLine <= rows; firstLine += BATCH)
                {
                    RowBatch batch = MakeStreamBatch(source, valueKey, firstLine, BATCH, firstLine == 1);
                    const auto start = std::chrono::steady_clock::now();
                    AppendFixtureBatch(table, std::move(batch));
                    if (table.RowCount() > keep)
                    {
                        table.EvictPrefixRows(table.RowCount() - keep);
                    }
                    timed += std::chrono::steady_clock::now() - start;
                }
                const bool correct = table.RowCount() == keep &&
                                     std::cmp_equal(std::get<std::int64_t>(table.GetValue(0, 0)), rows - keep + 1);
                return correct ? std::chrono::duration<double>(timed).count() : -1.0;
            };
            if (first)
            {
                streamOnce();
            }
            for (const auto _ : state)
            {
                const double seconds = streamOnce();
                if (seconds < 0.0)
                {
                    Fail(state, "the table did not keep the newest rows");
                    return;
                }
                state.SetIterationTime(seconds);
            }
            state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * rows));
        },
        LargeOptions{.repetitions = tier.largeRepetitions, .manualTime = true}
    );
}

} // namespace

void RegisterTableBenchmarks()
{
    const SharedMixed full = MakeSharedMixed(FULL_TIER);
    // A Time cell takes about 4 microseconds to format and a string sort about 3 microseconds a row, so those two use
    // fewer rows to stay within the time of one benchmark. Time cells show in the local time zone, where the first one
    // may still fall in 2025, so its check only looks for the century.
    constexpr std::size_t TIME_FORMAT_ROWS = 100'000;
    constexpr std::size_t STRING_SORT_ROWS = 250'000;
    RegisterFormatting(FULL_TIER, full, "Time", TIME_COLUMN, "20", TIME_FORMAT_ROWS, 1);
    RegisterFormatting(FULL_TIER, full, "integer", COUNT_COLUMN, "", FULL_TIER.rows, FAST_LARGE_ITERATIONS);
    RegisterFormatting(FULL_TIER, full, "double", RATIO_COLUMN, "", FULL_TIER.rows, FAST_LARGE_ITERATIONS);
    RegisterFormatting(FULL_TIER, full, "enumeration", REGION_COLUMN, "", FULL_TIER.rows, FAST_LARGE_ITERATIONS);

    RegisterSort(FULL_TIER, full, "Time column", TIME_COLUMN, true, FULL_TIER.rows);
    RegisterSort(FULL_TIER, full, "Time column", TIME_COLUMN, false, FULL_TIER.rows);
    RegisterSort(FULL_TIER, full, "string column", MESSAGE_COLUMN, true, STRING_SORT_ROWS);
    RegisterSort(FULL_TIER, full, "string column", MESSAGE_COLUMN, false, STRING_SORT_ROWS);

    RegisterSort(FULL_TIER, full, "integer column", COUNT_COLUMN, true, FULL_TIER.rows);
    RegisterSort(FULL_TIER, full, "integer column", COUNT_COLUMN, false, FULL_TIER.rows);
    RegisterSort(FULL_TIER, full, "double column", RATIO_COLUMN, true, FULL_TIER.rows);
    RegisterTopRows(FULL_TIER, full, "Time column", TIME_COLUMN, 100);

    // The text table has a path column whose cells share a long prefix and a column with doubled spaces.
    const SharedMixed text = MakeSharedTextMixed(FULL_TIER);
    RegisterSort(FULL_TIER, text, "string column with long common prefixes", PATH_COLUMN, true, FULL_TIER.rows);

    RegisterRegexFilter(FULL_TIER, full, "case-sensitive", CASE_SENSITIVE_PATTERN);
    RegisterRegexFilter(FULL_TIER, full, "case-insensitive", CASE_INSENSITIVE_PATTERN);

    // Contains on the column with doubled spaces: the filter must still find the timeout rows. Half of the cells need
    // their whitespace collapsed first.
    LeafRule contains;
    contains.type = LeafRule::Type::String;
    contains.columnKeys = {"spaced"};
    contains.matchType = LeafRule::Match::Contains;
    contains.filterString = "timeout after";
    RegisterLeafFilter(
        FULL_TIER, text, "Contains filter on cells with doubled spaces", contains, [](const MixedTable &fixture) {
            return fixture.timeoutRows;
        }
    );

    // The Time range covers the middle quarter of the day the times are spread over. The counts come from plain reads.
    constexpr std::int64_t DAY_START_MICROS = std::int64_t{1767225600} * 1'000'000;
    constexpr std::int64_t HOUR_MICROS = std::int64_t{3600} * 1'000'000;
    LeafRule timeRange;
    timeRange.type = LeafRule::Type::Time;
    timeRange.columnKeys = {"time"};
    timeRange.filterBegin = DAY_START_MICROS + (6 * HOUR_MICROS);
    timeRange.filterEnd = DAY_START_MICROS + (12 * HOUR_MICROS);
    RegisterLeafFilter(FULL_TIER, full, "Time range filter", timeRange, [timeRange](const MixedTable &fixture) {
        std::size_t count = 0;
        for (std::size_t row = 0; row < fixture.rows; ++row)
        {
            const auto &value = std::get<TimeStamp>(fixture.table->GetValue(row, TIME_COLUMN));
            const std::int64_t micros = value.time_since_epoch().count();
            count += micros >= *timeRange.filterBegin && micros <= *timeRange.filterEnd ? 1U : 0U;
        }
        return count;
    });

    LeafRule integerRange;
    integerRange.type = LeafRule::Type::Number;
    integerRange.columnKeys = {"count"};
    integerRange.filterMinValue = 250'000.0;
    integerRange.filterMaxValue = 500'000.0;
    RegisterLeafFilter(FULL_TIER, full, "Integer range filter", integerRange, [](const MixedTable &fixture) {
        std::size_t count = 0;
        for (std::size_t row = 0; row < fixture.rows; ++row)
        {
            const auto value = std::get<std::int64_t>(fixture.table->GetValue(row, COUNT_COLUMN));
            count += value >= 250'000 && value <= 500'000 ? 1U : 0U;
        }
        return count;
    });

    LeafRule doubleRange;
    doubleRange.type = LeafRule::Type::Number;
    doubleRange.columnKeys = {"ratio"};
    doubleRange.filterMinValue = 100.0;
    doubleRange.filterMaxValue = 400.0;
    RegisterLeafFilter(FULL_TIER, full, "Double range filter", doubleRange, [](const MixedTable &fixture) {
        std::size_t count = 0;
        for (std::size_t row = 0; row < fixture.rows; ++row)
        {
            const double value = std::get<double>(fixture.table->GetValue(row, RATIO_COLUMN));
            count += value >= 100.0 && value <= 400.0 ? 1U : 0U;
        }
        return count;
    });

    RegisterRetention(FULL_TIER);

    // The smoke tier has one sort, one filter and the stream, on a smaller table.
    const SharedMixed smoke = MakeSharedMixed(SMOKE_TIER);
    RegisterSort(SMOKE_TIER, smoke, "Time column", TIME_COLUMN, true, SMOKE_TIER.rows);
    RegisterRegexFilter(SMOKE_TIER, smoke, "case-sensitive", CASE_SENSITIVE_PATTERN);
    RegisterRetention(SMOKE_TIER);
}

} // namespace bench
