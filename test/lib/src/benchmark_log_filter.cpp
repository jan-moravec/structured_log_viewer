// loglib::RowPredicate + CompareRows micro-benchmarks: filtering, comparing and sorting 1'000'000-row tables. See
// CONTRIBUTING.md ## Benchmarking.
//
// The Type::Enumeration cases pin the column to a non-level key (region) so the auto-promotion heuristic in
// LogTable::AppendBatch cannot flip the column to Type::Level mid-fixture and silently change which
// CompareRows branch is under test. The Type::Level cases use the level key with a fully canonical six-alias
// vocabulary and pin the column type via the saved configuration so the path is exercised regardless of how the
// heuristic might evolve.

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"
#include "common.hpp"
#include "table_fixture.hpp"

#include <loglib/core/enum_dictionary.hpp>
#include <loglib/core/file_line_source.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_level.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/core/log_value.hpp>
#include <loglib/parse/log_parse_sink.hpp>
#include <loglib/query/log_compare.hpp>
#include <loglib/query/log_filter.hpp>
#include <loglib/schema/log_configuration.hpp>
#include <loglib/table/log_table.hpp>

#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace loglib;

namespace bench
{

namespace
{

LogLine MakeLine(KeyIndex &keys, LineSource &source, const std::vector<std::pair<std::string, LogValue>> &fields)
{
    std::vector<std::pair<KeyId, LogValue>> sorted;
    sorted.reserve(fields.size());
    for (const auto &[key, value] : fields)
    {
        sorted.emplace_back(keys.GetOrInsert(key), value);
    }
    std::ranges::sort(sorted, [](const auto &a, const auto &b) { return a.first < b.first; });
    return {std::move(sorted), source, 0};
}

struct LargeTable
{
    LogTable table;
    std::unique_ptr<FileLineSource> sourceOwner; // keeps the file mmap alive
};

/**
 * Build a Type::Enumeration LogTable with @p rowCount rows over a
 * fixed four-value vocabulary. The column key is taken from
 * @p columnKey so callers can pick one that does *not* match the
 * log-level field-name heuristic (e.g. region); otherwise the
 * auto-promotion logic on LogTable::AppendBatch would flip the
 * column to Type::Level once the dictionary fills with
 * canonical-level aliases, silently changing both the storage path
 * (CompareEnum -> CompareLevel) and the EnumDictRank semantics
 * the benchmarks below rely on. Used by the filter and enum-sort
 * benchmarks below.
 */
LargeTable BuildLargeEnumTable(const TestLogFile &fixture, size_t rowCount, std::string_view columnKey)
{
    auto source = std::make_unique<FileLineSource>(fixture.GetFilePath());
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = std::string(columnKey),
         .keys = {std::string(columnKey)},
         .printFormat = "{}",
         .type = ColumnType::Enumeration,
         .parseFormats = {},
         .levelMapping = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    loglib::LogTable table(mgr.Configuration());
    table.BeginStreaming(std::move(source));

    const std::vector<std::string> values = {"info", "warn", "error", "debug"};
    KeyIndex &keys = table.Keys();

    // Use a deterministic PRNG so successive runs have identical
    // layout. Reproducibility, not unpredictability, is the goal.
    // NOLINTNEXTLINE(cert-msc32-c, cert-msc51-cpp, bugprone-random-generator-seed)
    std::mt19937 rng{0xC0FFEEU};
    std::uniform_int_distribution<size_t> pick{0, values.size() - 1};

    const std::string columnKeyOwned(columnKey);
    constexpr size_t BATCH = 50'000;
    for (size_t base = 0; base < rowCount; base += BATCH)
    {
        const size_t batchSize = std::min(BATCH, rowCount - base);
        RowBatch batch;
        batch.firstLineNumber = base + 1;
        batch.lines.reserve(batchSize);
        for (size_t i = 0; i < batchSize; ++i)
        {
            batch.lines.push_back(MakeLine(keys, *sourcePtr, {{columnKeyOwned, std::string(values[pick(rng)])}}));
        }
        if (base == 0)
        {
            batch.newKeys.emplace_back(columnKeyOwned);
        }
        AppendFixtureBatch(table, std::move(batch));
    }
    return {.table = std::move(table), .sourceOwner = nullptr};
}

/**
 * Build a Type::Level LogTable with @p rowCount rows over the
 * canonical six-level vocabulary. The configuration pins the column
 * to Type::Level so the table does *not* depend on the
 * auto-promotion heuristic in LogTable::AppendBatch, which is
 * content-sensitive and can flip the column type mid-fixture as the
 * dictionary grows. The vocabulary deliberately covers all six
 * canonical levels so the resulting LevelRankCache exercises every
 * LogLevel ordinal -- otherwise a tbb::parallel_sort over
 * near-identical ranks would understate the per-compare cost. Used
 * by the level-sort benchmarks below.
 */
LargeTable BuildLargeLevelTable(const TestLogFile &fixture, size_t rowCount)
{
    auto source = std::make_unique<FileLineSource>(fixture.GetFilePath());
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "level",
         .keys = {"level"},
         .printFormat = "{}",
         .type = ColumnType::Level,
         .parseFormats = {},
         .levelMapping = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    loglib::LogTable table(mgr.Configuration());
    table.BeginStreaming(std::move(source));

    // All six canonical aliases so the LevelRankCache covers the
    // full Trace..Fatal range. The order is deliberately not the
    // canonical severity order so the parallel sort actually has
    // permutation work to do.
    const std::vector<std::string> values = {"info", "warn", "error", "debug", "trace", "fatal"};
    KeyIndex &keys = table.Keys();

    // NOLINTNEXTLINE(cert-msc32-c, cert-msc51-cpp, bugprone-random-generator-seed)
    std::mt19937 rng{0xC0FFEE2U};
    std::uniform_int_distribution<size_t> pick{0, values.size() - 1};

    constexpr size_t BATCH = 50'000;
    for (size_t base = 0; base < rowCount; base += BATCH)
    {
        const size_t batchSize = std::min(BATCH, rowCount - base);
        RowBatch batch;
        batch.firstLineNumber = base + 1;
        batch.lines.reserve(batchSize);
        for (size_t i = 0; i < batchSize; ++i)
        {
            batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"level", std::string(values[pick(rng)])}}));
        }
        if (base == 0)
        {
            batch.newKeys.emplace_back("level");
        }
        AppendFixtureBatch(table, std::move(batch));
    }
    return {.table = std::move(table), .sourceOwner = nullptr};
}

/**
 * Build a Type::String LogTable with @p rowCount rows over a
 * pool of short message templates. Used by the
 * CallbackStringRowPredicate benchmark below.
 */
LargeTable BuildLargeStringTable(const TestLogFile &fixture, size_t rowCount)
{
    auto source = std::make_unique<FileLineSource>(fixture.GetFilePath());
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "msg",
         .keys = {"msg"},
         .printFormat = "{}",
         .type = ColumnType::String,
         .parseFormats = {},
         .levelMapping = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    loglib::LogTable table(mgr.Configuration());
    table.BeginStreaming(std::move(source));

    // Pool: a handful of canned messages so the matcher sees both
    // hits and misses with a stable hit ratio. The benchmark needle
    // is "user-id" -- present in three of the seven templates.
    const std::vector<std::string> templates = {
        "request handled for user-id=42",
        "request handled for guest, no auth",
        "cache miss on key=session-token",
        "user-id=17 logged out cleanly",
        "background sweep complete; 0 items dropped",
        "auth/token refresh ok, user-id=99",
        "shutdown signal received, draining queues",
    };
    KeyIndex &keys = table.Keys();

    // NOLINTNEXTLINE(cert-msc32-c, cert-msc51-cpp, bugprone-random-generator-seed)
    std::mt19937 rng{0xABCDEF01U};
    std::uniform_int_distribution<size_t> pick{0, templates.size() - 1};

    constexpr size_t BATCH = 50'000;
    for (size_t base = 0; base < rowCount; base += BATCH)
    {
        const size_t batchSize = std::min(BATCH, rowCount - base);
        RowBatch batch;
        batch.firstLineNumber = base + 1;
        batch.lines.reserve(batchSize);
        for (size_t i = 0; i < batchSize; ++i)
        {
            batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"msg", std::string(templates[pick(rng)])}}));
        }
        if (base == 0)
        {
            batch.newKeys.emplace_back("msg");
        }
        AppendFixtureBatch(table, std::move(batch));
    }
    return {.table = std::move(table), .sourceOwner = nullptr};
}

constexpr std::size_t ROW_COUNT = 1'000'000;

// A 1'000'000-row table with its fixture file, built once and shared by the benchmarks that read it.
struct TableInput
{
    std::unique_ptr<TestLogFile> fixture;
    std::unique_ptr<LargeTable> owned;
    const EnumDictionary *dict = nullptr;
    std::unique_ptr<EnumDictRank> rank;

    LogTable &Table() const
    {
        return owned->table;
    }
};

std::unique_ptr<TableInput> MakeEnumInput(const char *fileName)
{
    auto input = std::make_unique<TableInput>();
    input->fixture = std::make_unique<TestLogFile>(fileName);
    input->fixture->Write("");
    // region is not a known level key, so the column stays Type::Enumeration and exercises EnumRowPredicate.
    input->owned = std::make_unique<LargeTable>(BuildLargeEnumTable(*input->fixture, ROW_COUNT, "region"));
    LogTable &table = input->Table();
    const KeyId regionKey = table.Keys().Find("region");
    if (table.RowCount() != ROW_COUNT || regionKey == INVALID_KEY_ID)
    {
        throw std::runtime_error("the enum table was not built");
    }
    input->dict = table.EnumDictionaries().Find(regionKey);
    if (input->dict == nullptr)
    {
        throw std::runtime_error("the enum table has no dictionary");
    }
    input->rank = std::make_unique<EnumDictRank>(*input->dict);
    return input;
}

std::unique_ptr<TableInput> MakeLevelInput(const char *fileName)
{
    auto input = std::make_unique<TableInput>();
    input->fixture = std::make_unique<TestLogFile>(fileName);
    input->fixture->Write("");
    input->owned = std::make_unique<LargeTable>(BuildLargeLevelTable(*input->fixture, ROW_COUNT));
    LogTable &table = input->Table();
    if (table.RowCount() != ROW_COUNT || table.Keys().Find("level") == INVALID_KEY_ID ||
        table.Configuration().Configuration().columns[0].type != ColumnType::Level)
    {
        throw std::runtime_error("the level table was not built");
    }
    return input;
}

std::unique_ptr<TableInput> MakeStringInput(const char *fileName)
{
    auto input = std::make_unique<TableInput>();
    input->fixture = std::make_unique<TestLogFile>(fileName);
    input->fixture->Write("");
    input->owned = std::make_unique<LargeTable>(BuildLargeStringTable(*input->fixture, ROW_COUNT));
    if (input->Table().RowCount() != ROW_COUNT)
    {
        throw std::runtime_error("the string table was not built");
    }
    return input;
}

// Build the same enum leaf shape the filter tests do, but laid out inline so the benchmark file stays self-contained.
[[nodiscard]] CompiledFilterExpression MakeEnumLeaf(
    const LogTable &table, size_t columnIndex, const std::vector<std::string> &selectedOwned
)
{
    const KeyId key = table.Keys().Find(table.Configuration().Configuration().columns[columnIndex].keys.front());
    const EnumDictionary *dict = table.EnumDictionaries().Find(key);
    std::vector<std::string_view> views;
    views.reserve(selectedOwned.size());
    for (const auto &v : selectedOwned)
    {
        views.emplace_back(v);
    }
    CompiledFilterExpression expr;
    expr.node = CompiledFilterExpression::Leaf{
        RowPredicate{std::in_place_type<EnumRowPredicate>, columnIndex, std::span<const std::string_view>(views), dict}
    };
    expr.referencedColumns.push_back(columnIndex);
    return expr;
}

using SharedTable = std::shared_ptr<LazyInput<TableInput>>;

// Counts the rows a predicate accepts, one pass per iteration.
template <typename Predicate>
void RunPredicateWalk(benchmark::State &state, const LogTable &table, const Predicate &predicate, bool warmUp = false)
{
    auto walk = [&]() {
        std::size_t hits = 0;
        for (std::size_t row = 0; row < ROW_COUNT; ++row)
        {
            if (predicate.MatchesRow(table, row))
            {
                ++hits;
            }
        }
        return hits;
    };
    std::size_t accepted = warmUp ? walk() : 0;
    for (auto _ : state)
    {
        accepted = walk();
        benchmark::DoNotOptimize(accepted);
    }
    if (accepted == 0 || accepted >= ROW_COUNT)
    {
        Fail(state, "the predicate accepted none or all of the rows");
        return;
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * ROW_COUNT));
    state.counters["accepted"] = static_cast<double>(accepted);
}

// A shared table plus the scratch permutation a sort benchmark reuses.
struct TableScratch
{
    SharedTable table;
    std::vector<std::size_t> indices;
};

// Sorts a fresh identity permutation with CompareRows each iteration; the reset is not timed. The sorted order is
// checked after the timed part with inOrder(input, table, lhsRow, rhsRow).
template <typename Compare, typename InOrder>
void RegisterCompareRowsSort(const std::string &name, SharedTable table, Compare compare, InOrder inOrder)
{
    RegisterLarge<TableScratch>(
        name,
        [table]() {
            auto scratch = std::make_unique<TableScratch>();
            scratch->table = table;
            scratch->indices.resize(ROW_COUNT);
            return scratch;
        },
        [compare, inOrder](TableScratch &scratch, benchmark::State &state, bool first) {
            const TableInput &input = scratch.table->Get();
            const LogTable &logTable = input.Table();
            auto sortIdentity = [&]() {
                std::iota(scratch.indices.begin(), scratch.indices.end(), std::size_t{0});
                std::ranges::sort(scratch.indices, [&](std::size_t a, std::size_t b) {
                    return compare(input, logTable, a, b) < 0;
                });
            };
            if (first)
            {
                sortIdentity();
            }
            for (auto _ : state)
            {
                state.PauseTiming();
                std::iota(scratch.indices.begin(), scratch.indices.end(), std::size_t{0});
                state.ResumeTiming();
                std::ranges::sort(scratch.indices, [&](std::size_t a, std::size_t b) {
                    return compare(input, logTable, a, b) < 0;
                });
            }
            for (std::size_t i = 1; i < scratch.indices.size(); ++i)
            {
                if (!inOrder(input, logTable, scratch.indices[i - 1], scratch.indices[i]))
                {
                    Fail(state, "the sort did not produce ascending order");
                    return;
                }
            }
            state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * ROW_COUNT));
        }
    );
}

// Registers SortPermutationByColumn over @p table. Rank is the enum rank, or null for a level column, which reads its
// own rank cache the way the production proxy chain calls the helper.
void RegisterSortPermutation(const std::string &name, SharedTable table, bool useEnumRank)
{
    RegisterSmall(name, [table, useEnumRank](benchmark::State &state) {
        const TableInput &input = table->Get();
        const LogTable &logTable = input.Table();
        std::vector<std::size_t> logRows(ROW_COUNT);
        std::iota(logRows.begin(), logRows.end(), std::size_t{0});
        std::vector<std::size_t> permutation;
        for (auto _ : state)
        {
            permutation = SortPermutationByColumn(
                logTable,
                std::span<const std::size_t>{logRows},
                std::size_t{0},
                /*ascending=*/true,
                useEnumRank ? input.rank.get() : nullptr
            );
            benchmark::DoNotOptimize(permutation);
        }
        // Ascending order with an input-index tie-break (SortPermutationByColumn documents this stability).
        bool ordered = permutation.size() == ROW_COUNT;
        for (std::size_t i = 1; ordered && i < permutation.size(); ++i)
        {
            std::uint32_t lhsRank = 0;
            std::uint32_t rhsRank = 0;
            if (useEnumRank)
            {
                const auto lhsId = logTable.GetEnumValueId(logRows[permutation[i - 1]], 0);
                const auto rhsId = logTable.GetEnumValueId(logRows[permutation[i]], 0);
                if (!lhsId.has_value() || !rhsId.has_value())
                {
                    continue;
                }
                lhsRank = input.rank->RankOf(*lhsId);
                rhsRank = input.rank->RankOf(*rhsId);
            }
            else
            {
                const auto lhs = logTable.GetLevelForRow(logRows[permutation[i - 1]], 0);
                const auto rhs = logTable.GetLevelForRow(logRows[permutation[i]], 0);
                if (!lhs.has_value() || !rhs.has_value())
                {
                    continue;
                }
                lhsRank = static_cast<std::uint8_t>(*lhs);
                rhsRank = static_cast<std::uint8_t>(*rhs);
            }
            ordered = lhsRank <= rhsRank && (lhsRank != rhsRank || permutation[i - 1] < permutation[i]);
        }
        if (!ordered)
        {
            Fail(state, "the permutation is not sorted");
            return;
        }
        state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * ROW_COUNT));
    });
}

// Registers FilterAcceptedRows over a tree that @p makeExpression builds from the enum table.
template <typename MakeExpression>
void RegisterFilterAcceptedRows(
    const std::string &name, SharedTable table, MakeExpression makeExpression, bool expectAny
)
{
    RegisterSmall(name, [table, makeExpression, expectAny](benchmark::State &state) {
        const TableInput &input = table->Get();
        const LogTable &logTable = input.Table();
        const CompiledFilterExpression expr = makeExpression(logTable, input.dict);
        std::size_t accepted = 0;
        std::vector<std::size_t> result;
        for (auto _ : state)
        {
            result = FilterAcceptedRows(logTable, expr);
            accepted = result.size();
            benchmark::DoNotOptimize(result);
        }
        if (expectAny ? (accepted == 0 || accepted >= ROW_COUNT) : accepted != 0)
        {
            Fail(state, "the filter accepted an unexpected number of rows");
            return;
        }
        state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations() * ROW_COUNT));
        state.counters["accepted"] = static_cast<double>(accepted);
    });
}

} // namespace

void RegisterLogFilterBenchmarks()
{
    // Each table is built on first use and shared by the benchmarks that read it.
    const SharedTable enumTable =
        MakeLazy<TableInput>([]() { return MakeEnumInput("benchmark_log_filter_enum.json"); });
    const SharedTable levelTable =
        MakeLazy<TableInput>([]() { return MakeLevelInput("benchmark_log_filter_level.json"); });
    const SharedTable stringTable =
        MakeLazy<TableInput>([]() { return MakeStringInput("benchmark_log_filter_string.json"); });

    RegisterSmall("EnumRowPredicate over 1000000 rows", [enumTable](benchmark::State &state) {
        const TableInput &input = enumTable->Get();
        const std::vector<std::string> selectedOwned = {"warn", "error"};
        std::vector<std::string_view> selected(selectedOwned.begin(), selectedOwned.end());
        const EnumRowPredicate predicate(0, selected, input.dict);
        if (!predicate.IsFastPathArmed())
        {
            Fail(state, "the enum fast path is not armed");
            return;
        }
        RunPredicateWalk(state, input.Table(), predicate);
    });

    // Naive substring matcher; mirrors the cheapest callback the GUI ever installs (the Contains match type
    // without case-folding). Captured by value so the predicate owns its own state.
    RegisterLarge<TableScratch>(
        "CallbackStringRowPredicate over 1000000 rows",
        [stringTable]() {
            auto scratch = std::make_unique<TableScratch>();
            scratch->table = stringTable;
            return scratch;
        },
        [](TableScratch &scratch, benchmark::State &state, bool first) {
            const TableInput &input = scratch.table->Get();
            const std::string needle = "user-id";
            // clang-tidy mistakes the lambda's implicit copy constructor (which allocates when duplicating the
            // captured std::string) for an exception escaping the call operator; the operator only does a
            // string_view::contains.
            CallbackStringRowPredicate predicate(
                0,
                // NOLINTNEXTLINE(bugprone-exception-escape)
                [needle](std::string_view slot) { return slot.contains(needle); }
            );
            RunPredicateWalk(state, input.Table(), predicate, first);
        },
        LargeOptions{}
    );

    RegisterCompareRowsSort(
        "CompareRows enum sort over 1000000 rows",
        enumTable,
        [](const TableInput &input, const LogTable &table, std::size_t a, std::size_t b) {
            return CompareRows(table, a, b, 0, input.rank.get());
        },
        [](const TableInput &input, const LogTable &table, std::size_t lhsRow, std::size_t rhsRow) {
            const auto lhs = table.GetEnumValueId(lhsRow, 0);
            const auto rhs = table.GetEnumValueId(rhsRow, 0);
            return !lhs.has_value() || !rhs.has_value() || input.rank->RankOf(*lhs) <= input.rank->RankOf(*rhs);
        }
    );

    RegisterFilterAcceptedRows(
        "FilterAcceptedRows (parallel) over 1000000 rows",
        enumTable,
        [](const LogTable &, const EnumDictionary *dict) {
            const std::vector<std::string> selectedOwned = {"warn", "error"};
            std::vector<std::string_view> selected(selectedOwned.begin(), selectedOwned.end());
            // Wrap the single predicate in a CompiledFilterExpression::Leaf so the benchmark exercises the same
            // evaluator the app takes.
            CompiledFilterExpression compiled;
            compiled.node = CompiledFilterExpression::Leaf{RowPredicate{
                std::in_place_type<EnumRowPredicate>, size_t{0}, std::span<const std::string_view>(selected), dict
            }};
            compiled.referencedColumns.push_back(size_t{0});
            return compiled;
        },
        /*expectAny=*/true
    );

    RegisterSortPermutation("SortPermutationByColumn (parallel + pre-mat rank) over 1000000 rows", enumTable, true);

    // Level-column siblings of the enum benchmarks. They guard the pre-materialised uint8_t rank fast path in
    // SortPermutationByColumn and the CompareLevel -> GetLevelForRow dispatch in CompareRows. The order checks
    // use LogLevel (canonical severity) rather than alphabetic rank.
    RegisterSortPermutation(
        "SortPermutationByColumn (parallel + pre-mat level rank) over 1000000 rows", levelTable, false
    );

    RegisterCompareRowsSort(
        "CompareRows level sort over 1000000 rows",
        levelTable,
        [](const TableInput &, const LogTable &table, std::size_t a, std::size_t b) {
            // Level columns ignore the EnumDictRank argument; null matches a non-Qt consumer that has not
            // materialised any rank table.
            return CompareRows(table, a, b, 0, nullptr);
        },
        [](const TableInput &, const LogTable &table, std::size_t lhsRow, std::size_t rhsRow) {
            const auto lhs = table.GetLevelForRow(lhsRow, 0);
            const auto rhs = table.GetLevelForRow(rhsRow, 0);
            return !lhs.has_value() || !rhs.has_value() ||
                   static_cast<std::uint8_t>(*lhs) <= static_cast<std::uint8_t>(*rhs);
        }
    );

    // Boolean-expression benchmarks: FilterAcceptedRows on trees more interesting than a single leaf.
    //
    // 1. Deep AND -- four enum leaves conjoined. This is what "multiple simple filters" produce; the visit path
    //    evaluates it row by row with a natural short-circuit at the first rejecting leaf, which is why
    //    ShouldUseBitsetPath deliberately leaves flat And trees alone no matter how many leaves they carry. The
    //    four canonical values are disjoint, so no row is accepted; the purpose is timing shape, not the count.
    RegisterFilterAcceptedRows(
        "FilterAcceptedRows AND(4 enum leaves) over 1000000 rows",
        enumTable,
        [](const LogTable &table, const EnumDictionary *) {
            std::vector<CompiledFilterExpression> children;
            children.push_back(MakeEnumLeaf(table, 0, {"info"}));
            children.push_back(MakeEnumLeaf(table, 0, {"warn"}));
            children.push_back(MakeEnumLeaf(table, 0, {"error"}));
            children.push_back(MakeEnumLeaf(table, 0, {"debug"}));
            CompiledFilterExpression expr;
            CompiledFilterExpression::And andNode;
            andNode.children = std::move(children);
            expr.node = std::move(andNode);
            expr.referencedColumns.push_back(0);
            return expr;
        },
        /*expectAny=*/false
    );

    // 2. AND(OR, NOT) -- the shape a level in {...} AND NOT path:health query compiles to. It has both Or and
    //    Not, so the bitset-materialisation heuristic in FilterAcceptedRows engages: each unique leaf becomes a
    //    RowBitset, then the tree collapses via word-parallel & / | / ~. Here: (region in {warn, error,
    //    debug}) AND NOT region=debug.
    RegisterFilterAcceptedRows(
        "FilterAcceptedRows AND(OR, NOT) [bitset path] over 1000000 rows",
        enumTable,
        [](const LogTable &table, const EnumDictionary *) {
            std::vector<CompiledFilterExpression> orChildren;
            orChildren.push_back(MakeEnumLeaf(table, 0, {"warn"}));
            orChildren.push_back(MakeEnumLeaf(table, 0, {"error"}));
            orChildren.push_back(MakeEnumLeaf(table, 0, {"debug"}));
            CompiledFilterExpression orExpr;
            CompiledFilterExpression::Or orNode;
            orNode.children = std::move(orChildren);
            orExpr.node = std::move(orNode);
            orExpr.referencedColumns.push_back(0);

            CompiledFilterExpression notExpr;
            notExpr.node = CompiledFilterExpression::Not{MakeEnumLeaf(table, 0, {"debug"})};
            notExpr.referencedColumns.push_back(0);

            std::vector<CompiledFilterExpression> andChildren;
            andChildren.push_back(std::move(orExpr));
            andChildren.push_back(std::move(notExpr));
            CompiledFilterExpression expr;
            CompiledFilterExpression::And andNode;
            andNode.children = std::move(andChildren);
            expr.node = std::move(andNode);
            expr.referencedColumns.push_back(0);
            return expr;
        },
        /*expectAny=*/true
    );
}

} // namespace bench
