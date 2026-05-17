// `loglib::RowPredicate` + `CompareRows` micro-benchmarks. Mirrors the
// shape of the existing `[large]` JSON benchmarks: tagged
// `[.][benchmark][log_filter][large]` (with `[level]` added on the
// `Type::Level` siblings) so they land under the `benchmark` CTest
// label and are hidden from the default run. Numbers are reported via
// `WARN` so they print on success and end up in the CI log lines the
// comparison script scrapes.
//
// The `Type::Enumeration` cases pin the column to a non-level key
// (`region`) so the auto-promotion heuristic in
// `LogTable::AppendBatch` cannot flip the column to `Type::Level` mid-
// fixture and silently change which `CompareRows` branch is under
// test. The `Type::Level` cases use the `level` key with a fully
// canonical six-alias vocabulary and pin the column type via the
// saved configuration so the path is exercised regardless of how the
// heuristic might evolve.

#include "common.hpp"

#include <loglib/enum_dictionary.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_compare.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_filter.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_table.hpp>
#include <loglib/log_value.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

using namespace loglib;

namespace
{

inline void RequireReleaseBuildForBenchmarks()
{
#ifndef NDEBUG
    SKIP("Benchmarks require a release build (Debug disables IPO/LTO and "
         "leaves assertions enabled, so numbers are not comparable). "
         "Rebuild with: cmake --preset release  (or relwithdebinfo).");
#endif
}

LogLine MakeLine(KeyIndex &keys, LineSource &source, const std::vector<std::pair<std::string, LogValue>> &fields)
{
    std::vector<std::pair<KeyId, LogValue>> sorted;
    sorted.reserve(fields.size());
    for (const auto &[key, value] : fields)
    {
        sorted.emplace_back(keys.GetOrInsert(key), value);
    }
    std::ranges::sort(sorted, [](const auto &a, const auto &b) { return a.first < b.first; });
    return {std::move(sorted), keys, source, 0};
}

struct LargeTable
{
    LogTable table;
    std::unique_ptr<FileLineSource> sourceOwner; // keeps the file mmap alive
};

/// Build a `Type::Enumeration` `LogTable` with @p rowCount rows over a
/// fixed four-value vocabulary. The column key is taken from
/// @p columnKey so callers can pick one that does *not* match the
/// log-level field-name heuristic (e.g. `region`); otherwise the
/// auto-promotion logic on `LogTable::AppendBatch` would flip the
/// column to `Type::Level` once the dictionary fills with
/// canonical-level aliases, silently changing both the storage path
/// (`CompareEnum` -> `CompareLevel`) and the `EnumDictRank` semantics
/// the benchmarks below rely on. Used by the filter and enum-sort
/// benchmarks below.
LargeTable BuildLargeEnumTable(const TestLogFile &fixture, size_t rowCount, std::string_view columnKey)
{
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(fixture.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = std::string(columnKey),
         .keys = {std::string(columnKey)},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Enumeration,
         .parseFormats = {},
         .levelMapping = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
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
        StreamedBatch batch;
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
        table.AppendBatch(std::move(batch));
    }
    return {.table = std::move(table), .sourceOwner = nullptr};
}

/// Build a `Type::Level` `LogTable` with @p rowCount rows over the
/// canonical six-level vocabulary. The configuration pins the column
/// to `Type::Level` so the table does *not* depend on the
/// auto-promotion heuristic in `LogTable::AppendBatch`, which is
/// content-sensitive and can flip the column type mid-fixture as the
/// dictionary grows. The vocabulary deliberately covers all six
/// canonical levels so the resulting `LevelRankCache` exercises every
/// `LogLevel` ordinal -- otherwise a `tbb::parallel_sort` over
/// near-identical ranks would understate the per-compare cost. Used
/// by the level-sort benchmarks below.
LargeTable BuildLargeLevelTable(const TestLogFile &fixture, size_t rowCount)
{
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(fixture.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "level",
         .keys = {"level"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Level,
         .parseFormats = {},
         .levelMapping = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));

    // All six canonical aliases so the `LevelRankCache` covers the
    // full `Trace`..`Fatal` range. The order is deliberately not the
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
        StreamedBatch batch;
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
        table.AppendBatch(std::move(batch));
    }
    return {.table = std::move(table), .sourceOwner = nullptr};
}

/// Build a `Type::String` `LogTable` with @p rowCount rows over a
/// pool of short message templates. Used by the
/// `CallbackStringRowPredicate` benchmark below.
LargeTable BuildLargeStringTable(const TestLogFile &fixture, size_t rowCount)
{
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(fixture.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "msg",
         .keys = {"msg"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::String,
         .parseFormats = {},
         .levelMapping = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
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
        StreamedBatch batch;
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
        table.AppendBatch(std::move(batch));
    }
    return {.table = std::move(table), .sourceOwner = nullptr};
}

template <typename Fn> std::chrono::nanoseconds TimeOnce(Fn fn)
{
    const auto start = std::chrono::steady_clock::now();
    fn();
    return std::chrono::steady_clock::now() - start;
}

} // namespace

TEST_CASE("RowPredicate enum filter walks 1'000'000 rows under 100ms", "[.][benchmark][log_filter][large]")
{
    RequireReleaseBuildForBenchmarks();

    constexpr size_t ROW_COUNT = 1'000'000;
    const TestLogFile fixture("benchmark_log_filter_enum.json");
    fixture.Write("");
    // `region` is not in the log-level field-name heuristic, so the
    // column stays `Type::Enumeration` regardless of dictionary content
    // and the predicate exercises the `EnumRowPredicate` fast path.
    LargeTable owned = BuildLargeEnumTable(fixture, ROW_COUNT, "region");
    LogTable &table = owned.table;
    REQUIRE(table.RowCount() == ROW_COUNT);

    const KeyId regionKey = table.Keys().Find("region");
    REQUIRE(regionKey != INVALID_KEY_ID);
    const EnumDictionary *dict = table.EnumDictionaries().Find(regionKey);
    REQUIRE(dict != nullptr);

    const std::vector<std::string> selectedOwned = {"warn", "error"};
    std::vector<std::string_view> selected;
    selected.reserve(selectedOwned.size());
    for (const auto &v : selectedOwned)
    {
        selected.emplace_back(v);
    }
    const EnumRowPredicate predicate(0, selected, dict);
    REQUIRE(predicate.IsFastPathArmed());

    constexpr int SAMPLES = 5;
    std::vector<std::chrono::nanoseconds> elapsed;
    elapsed.reserve(SAMPLES);
    size_t accepted = 0;
    for (int s = 0; s < SAMPLES; ++s)
    {
        size_t hits = 0;
        elapsed.push_back(TimeOnce([&]() {
            for (size_t row = 0; row < ROW_COUNT; ++row)
            {
                if (predicate.MatchesRow(table, row))
                {
                    ++hits;
                }
            }
        }));
        accepted = hits;
    }
    REQUIRE(accepted > 0);
    REQUIRE(accepted < ROW_COUNT);

    using Ms = std::chrono::duration<double, std::milli>;
    const auto mean = std::accumulate(elapsed.begin(), elapsed.end(), std::chrono::nanoseconds::zero()) /
                      static_cast<long long>(SAMPLES);
    const auto low = *std::ranges::min_element(elapsed);
    const auto high = *std::ranges::max_element(elapsed);

    WARN(
        "EnumRowPredicate over " << ROW_COUNT << " rows: mean=" << Ms(mean).count() << " ms (low=" << Ms(low).count()
                                 << ", high=" << Ms(high).count() << "), accepted=" << accepted
    );

    // Generous ceiling; the GUI's hard freeze symptom was several
    // seconds. Hitting >100 ms here suggests the fast path regressed
    // back to a per-row allocation, which is the regression we
    // explicitly designed against.
    CHECK(Ms(low).count() < 100.0);
}

TEST_CASE(
    "CallbackStringRowPredicate substring scan over 1'000'000 rows stays under 200ms",
    "[.][benchmark][log_filter][large]"
)
{
    RequireReleaseBuildForBenchmarks();

    constexpr size_t ROW_COUNT = 1'000'000;
    const TestLogFile fixture("benchmark_log_filter_string.json");
    fixture.Write("");
    LargeTable owned = BuildLargeStringTable(fixture, ROW_COUNT);
    LogTable &table = owned.table;
    REQUIRE(table.RowCount() == ROW_COUNT);

    // Naive substring matcher; mirrors the cheapest callback the
    // GUI ever installs (the `Contains` match type without
    // case-folding). Captured by value so the predicate owns its
    // own state.
    const std::string needle = "user-id";
    // clang-tidy mistakes the lambda's implicit copy constructor (which allocates when duplicating
    // the captured `std::string`) for an exception escaping the call operator; the operator only
    // does a `string_view::contains`.
    CallbackStringRowPredicate predicate(
        0,
        // NOLINTNEXTLINE(bugprone-exception-escape)
        [needle](std::string_view slot) { return slot.contains(needle); }
    );

    constexpr int SAMPLES = 5;
    std::vector<std::chrono::nanoseconds> elapsed;
    elapsed.reserve(SAMPLES);
    size_t accepted = 0;
    for (int s = 0; s < SAMPLES; ++s)
    {
        size_t hits = 0;
        elapsed.push_back(TimeOnce([&]() {
            for (size_t row = 0; row < ROW_COUNT; ++row)
            {
                if (predicate.MatchesRow(table, row))
                {
                    ++hits;
                }
            }
        }));
        accepted = hits;
    }
    REQUIRE(accepted > 0);
    REQUIRE(accepted < ROW_COUNT);

    using Ms = std::chrono::duration<double, std::milli>;
    const auto mean = std::accumulate(elapsed.begin(), elapsed.end(), std::chrono::nanoseconds::zero()) /
                      static_cast<long long>(SAMPLES);
    const auto low = *std::ranges::min_element(elapsed);
    const auto high = *std::ranges::max_element(elapsed);

    WARN(
        "CallbackStringRowPredicate over " << ROW_COUNT << " rows: mean=" << Ms(mean).count()
                                           << " ms (low=" << Ms(low).count() << ", high=" << Ms(high).count()
                                           << "), accepted=" << accepted
    );

    // Generous ceiling. The callback itself is `std::string::find`
    // on a short needle; the per-row cost is dominated by the table
    // lookup, not the substring scan. Going above 200 ms here flags
    // a regression in either the variant access or the table
    // bookkeeping.
    CHECK(Ms(low).count() < 200.0);
}

TEST_CASE(
    "CompareRows sort over enum column on 1'000'000 rows scales linearly",
    "[.][benchmark][log_filter][log_compare][large]"
)
{
    RequireReleaseBuildForBenchmarks();

    constexpr size_t ROW_COUNT = 1'000'000;
    const TestLogFile fixture("benchmark_log_compare_enum.json");
    fixture.Write("");
    // `region` keeps the column pinned at `Type::Enumeration`; with the
    // historical `level` key the auto-promotion heuristic flips the
    // column to `Type::Level` mid-build and `CompareRows` then ignores
    // the precomputed `EnumDictRank`, both inflating the per-compare
    // cost and breaking the rank-monotonic sanity check below.
    LargeTable owned = BuildLargeEnumTable(fixture, ROW_COUNT, "region");
    LogTable &table = owned.table;
    REQUIRE(table.RowCount() == ROW_COUNT);

    const KeyId regionKey = table.Keys().Find("region");
    REQUIRE(regionKey != INVALID_KEY_ID);
    const EnumDictionary *dict = table.EnumDictionaries().Find(regionKey);
    REQUIRE(dict != nullptr);
    const EnumDictRank rank{*dict};

    std::vector<size_t> indices(ROW_COUNT);
    // `std::iota` rather than `std::ranges::iota`: the latter is C++23
    // and AppleClang 17's libc++ still lacks it (macOS CI build).
    std::iota(indices.begin(), indices.end(), size_t{0});

    constexpr int SAMPLES = 3;
    std::vector<std::chrono::nanoseconds> elapsed;
    elapsed.reserve(SAMPLES);
    for (int s = 0; s < SAMPLES; ++s)
    {
        // Reset the permutation before each timed sort.
        std::iota(indices.begin(), indices.end(), size_t{0});
        elapsed.push_back(TimeOnce([&]() {
            std::ranges::sort(indices, [&](size_t a, size_t b) { return CompareRows(table, a, b, 0, &rank) < 0; });
        }));
    }

    using Ms = std::chrono::duration<double, std::milli>;
    const auto mean = std::accumulate(elapsed.begin(), elapsed.end(), std::chrono::nanoseconds::zero()) /
                      static_cast<long long>(SAMPLES);
    const auto low = *std::ranges::min_element(elapsed);
    const auto high = *std::ranges::max_element(elapsed);
    WARN(
        "CompareRows enum sort over " << ROW_COUNT << " rows: mean=" << Ms(mean).count()
                                      << " ms (low=" << Ms(low).count() << ", high=" << Ms(high).count() << ")"
    );

    // O(N log N) compares of a uint16_t rank should be well under a
    // second on a modern desktop; previous QVariant-mediated paths took
    // several seconds in the GUI for the same input.
    CHECK(Ms(low).count() < 1500.0);

    // Sanity: the sort really did produce ascending ranks.
    for (size_t i = 1; i < indices.size(); ++i)
    {
        const auto lhs = table.GetEnumValueId(indices[i - 1], 0);
        const auto rhs = table.GetEnumValueId(indices[i], 0);
        if (lhs.has_value() && rhs.has_value())
        {
            REQUIRE(rank.RankOf(*lhs) <= rank.RankOf(*rhs));
        }
    }
}

TEST_CASE("loglib::FilterAcceptedRows over 1'000'000 enum rows stays under 100ms", "[.][benchmark][log_filter][large]")
{
    RequireReleaseBuildForBenchmarks();

    constexpr size_t ROW_COUNT = 1'000'000;
    const TestLogFile fixture("benchmark_log_filter_parallel.json");
    fixture.Write("");
    LargeTable owned = BuildLargeEnumTable(fixture, ROW_COUNT, "region");
    LogTable &table = owned.table;
    REQUIRE(table.RowCount() == ROW_COUNT);

    const KeyId regionKey = table.Keys().Find("region");
    REQUIRE(regionKey != INVALID_KEY_ID);
    const EnumDictionary *dict = table.EnumDictionaries().Find(regionKey);
    REQUIRE(dict != nullptr);

    const std::vector<std::string> selectedOwned = {"warn", "error"};
    std::vector<std::string_view> selected;
    selected.reserve(selectedOwned.size());
    for (const auto &v : selectedOwned)
    {
        selected.emplace_back(v);
    }
    std::vector<RowPredicate> predicates;
    predicates.emplace_back(
        std::in_place_type<EnumRowPredicate>, size_t{0}, std::span<const std::string_view>(selected), dict
    );

    constexpr int SAMPLES = 5;
    std::vector<std::chrono::nanoseconds> elapsed;
    elapsed.reserve(SAMPLES);
    size_t accepted = 0;
    for (int s = 0; s < SAMPLES; ++s)
    {
        std::vector<size_t> result;
        elapsed.push_back(TimeOnce([&]() {
            result = FilterAcceptedRows(table, std::span<const RowPredicate>{predicates});
        }));
        accepted = result.size();
    }
    REQUIRE(accepted > 0);
    REQUIRE(accepted < ROW_COUNT);

    using Ms = std::chrono::duration<double, std::milli>;
    const auto mean = std::accumulate(elapsed.begin(), elapsed.end(), std::chrono::nanoseconds::zero()) /
                      static_cast<long long>(SAMPLES);
    const auto low = *std::ranges::min_element(elapsed);
    const auto high = *std::ranges::max_element(elapsed);

    WARN(
        "FilterAcceptedRows (parallel) over " << ROW_COUNT << " rows: mean=" << Ms(mean).count()
                                              << " ms (low=" << Ms(low).count() << ", high=" << Ms(high).count()
                                              << "), accepted=" << accepted
    );

    // Sequential reference lands ~16 ms on the dev box; the parallel
    // pass should stay within ~6x that even on a single-core CI
    // runner (TBB falls back to sequential without worker threads).
    // 100 ms tolerates CI variance while catching order-of-magnitude
    // regressions.
    CHECK(Ms(low).count() < 100.0);
}

TEST_CASE(
    "loglib::SortPermutationByColumn over 1'000'000 enum rows with rank stays under 500ms",
    "[.][benchmark][log_filter][log_compare][large]"
)
{
    RequireReleaseBuildForBenchmarks();

    constexpr size_t ROW_COUNT = 1'000'000;
    const TestLogFile fixture("benchmark_log_sort_parallel.json");
    fixture.Write("");
    LargeTable owned = BuildLargeEnumTable(fixture, ROW_COUNT, "region");
    LogTable &table = owned.table;
    REQUIRE(table.RowCount() == ROW_COUNT);

    const KeyId regionKey = table.Keys().Find("region");
    REQUIRE(regionKey != INVALID_KEY_ID);
    const EnumDictionary *dict = table.EnumDictionaries().Find(regionKey);
    REQUIRE(dict != nullptr);
    const EnumDictRank rank{*dict};

    std::vector<size_t> logRows(ROW_COUNT);
    std::iota(logRows.begin(), logRows.end(), size_t{0});

    constexpr int SAMPLES = 3;
    std::vector<std::chrono::nanoseconds> elapsed;
    elapsed.reserve(SAMPLES);
    std::vector<size_t> permutation;
    for (int s = 0; s < SAMPLES; ++s)
    {
        elapsed.push_back(TimeOnce([&]() {
            permutation =
                SortPermutationByColumn(table, std::span<const size_t>{logRows}, size_t{0}, /*ascending=*/true, &rank);
        }));
    }
    REQUIRE(permutation.size() == ROW_COUNT);

    using Ms = std::chrono::duration<double, std::milli>;
    const auto mean = std::accumulate(elapsed.begin(), elapsed.end(), std::chrono::nanoseconds::zero()) /
                      static_cast<long long>(SAMPLES);
    const auto low = *std::ranges::min_element(elapsed);
    const auto high = *std::ranges::max_element(elapsed);

    WARN(
        "SortPermutationByColumn (parallel + pre-mat rank) over " << ROW_COUNT << " rows: mean=" << Ms(mean).count()
                                                                  << " ms (low=" << Ms(low).count()
                                                                  << ", high=" << Ms(high).count() << ")"
    );

    // Sequential `std::ranges::sort + CompareRows` reference lands
    // ~148 ms on the dev box; parallel + pre-materialised rank is
    // ~68 ms. 500 ms is the CI ceiling: a regression past it means
    // either rank pre-materialisation broke or `tbb::parallel_sort`
    // collapsed back to sequential.
    CHECK(Ms(low).count() < 500.0);

    // Sanity: ascending rank order with input-index tie-break
    // (`SortPermutationByColumn` documents this stability).
    for (size_t i = 1; i < permutation.size(); ++i)
    {
        const auto lhsId = table.GetEnumValueId(logRows[permutation[i - 1]], 0);
        const auto rhsId = table.GetEnumValueId(logRows[permutation[i]], 0);
        if (lhsId.has_value() && rhsId.has_value())
        {
            const auto lhsRank = rank.RankOf(*lhsId);
            const auto rhsRank = rank.RankOf(*rhsId);
            REQUIRE(lhsRank <= rhsRank);
            if (lhsRank == rhsRank)
            {
                REQUIRE(permutation[i - 1] < permutation[i]);
            }
        }
    }
}

// --- `Type::Level` benchmarks --------------------------------------------
//
// The two cases below are the level-column siblings of the enum
// benchmarks above. They guard against regressions in the level path
// added by the log-level-detection feature: `SortPermutationByColumn`
// now pre-materialises a `uint8_t` `LogLevel` ordinal per row in
// parallel for `Type::Level` columns (mirroring the `Type::Enumeration`
// `EnumDictRank` fast path), and `CompareRows` dispatches level
// columns to `CompareLevel` which calls `LogTable::GetLevelForRow` per
// side. The sanity check is `LogLevel`-based here rather than
// alphabetic-rank-based so the canonical-severity ordering the level
// path sorts by is what we actually verify.

TEST_CASE(
    "loglib::SortPermutationByColumn over 1'000'000 level rows stays under 500ms",
    "[.][benchmark][log_filter][log_compare][large][level]"
)
{
    RequireReleaseBuildForBenchmarks();

    constexpr size_t ROW_COUNT = 1'000'000;
    const TestLogFile fixture("benchmark_log_sort_level_parallel.json");
    fixture.Write("");
    LargeTable owned = BuildLargeLevelTable(fixture, ROW_COUNT);
    LogTable &table = owned.table;
    REQUIRE(table.RowCount() == ROW_COUNT);

    const KeyId levelKey = table.Keys().Find("level");
    REQUIRE(levelKey != INVALID_KEY_ID);
    REQUIRE(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Level);

    std::vector<size_t> logRows(ROW_COUNT);
    std::iota(logRows.begin(), logRows.end(), size_t{0});

    constexpr int SAMPLES = 3;
    std::vector<std::chrono::nanoseconds> elapsed;
    elapsed.reserve(SAMPLES);
    std::vector<size_t> permutation;
    for (int s = 0; s < SAMPLES; ++s)
    {
        elapsed.push_back(TimeOnce([&]() {
            // Level columns ignore the rank parameter inside
            // `SortPermutationByColumn` -- the function consults
            // `LogTable::LevelRankCache(columnIndex)` directly. Pass
            // `nullptr` so the call shape mirrors how the production
            // proxy chain invokes the helper for level columns.
            permutation =
                SortPermutationByColumn(table, std::span<const size_t>{logRows}, size_t{0}, /*ascending=*/true, nullptr);
        }));
    }
    REQUIRE(permutation.size() == ROW_COUNT);

    using Ms = std::chrono::duration<double, std::milli>;
    const auto mean = std::accumulate(elapsed.begin(), elapsed.end(), std::chrono::nanoseconds::zero()) /
                      static_cast<long long>(SAMPLES);
    const auto low = *std::ranges::min_element(elapsed);
    const auto high = *std::ranges::max_element(elapsed);

    WARN(
        "SortPermutationByColumn (parallel + pre-mat level rank) over " << ROW_COUNT
                                                                        << " rows: mean=" << Ms(mean).count()
                                                                        << " ms (low=" << Ms(low).count()
                                                                        << ", high=" << Ms(high).count() << ")"
    );

    // The level fast path materialises `uint8_t` ordinals (vs
    // `uint16_t` for the enum path), so the per-row work is slightly
    // cheaper on its own; total wall-time is dominated by the
    // `tbb::parallel_sort` over an 8-bit key, which has hit ~30 ms
    // locally. Same 500 ms CI ceiling as the enum sibling so a
    // regression that disables either parallelism or the rank cache
    // surfaces immediately.
    CHECK(Ms(low).count() < 500.0);

    // Sanity: canonical severity is non-decreasing with input-index
    // tie-break. Reading via `GetLevelForRow` rather than the rank
    // cache directly so the check exercises the public read API the
    // GUI uses too.
    for (size_t i = 1; i < permutation.size(); ++i)
    {
        const auto lhs = table.GetLevelForRow(logRows[permutation[i - 1]], 0);
        const auto rhs = table.GetLevelForRow(logRows[permutation[i]], 0);
        if (lhs.has_value() && rhs.has_value())
        {
            const auto lhsRank = static_cast<uint8_t>(*lhs);
            const auto rhsRank = static_cast<uint8_t>(*rhs);
            REQUIRE(lhsRank <= rhsRank);
            if (lhsRank == rhsRank)
            {
                REQUIRE(permutation[i - 1] < permutation[i]);
            }
        }
    }
}

TEST_CASE(
    "CompareRows sort over level column on 1'000'000 rows scales linearly",
    "[.][benchmark][log_filter][log_compare][large][level]"
)
{
    RequireReleaseBuildForBenchmarks();

    constexpr size_t ROW_COUNT = 1'000'000;
    const TestLogFile fixture("benchmark_log_compare_level.json");
    fixture.Write("");
    LargeTable owned = BuildLargeLevelTable(fixture, ROW_COUNT);
    LogTable &table = owned.table;
    REQUIRE(table.RowCount() == ROW_COUNT);
    REQUIRE(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Level);

    std::vector<size_t> indices(ROW_COUNT);
    std::iota(indices.begin(), indices.end(), size_t{0});

    constexpr int SAMPLES = 3;
    std::vector<std::chrono::nanoseconds> elapsed;
    elapsed.reserve(SAMPLES);
    for (int s = 0; s < SAMPLES; ++s)
    {
        std::iota(indices.begin(), indices.end(), size_t{0});
        elapsed.push_back(TimeOnce([&]() {
            // Level columns ignore the `EnumDictRank` argument of
            // `CompareRows`; pass `nullptr` so the call shape matches
            // how a non-Qt `loglib` consumer would sort a level
            // column without first materialising a rank table.
            std::ranges::sort(indices, [&](size_t a, size_t b) { return CompareRows(table, a, b, 0, nullptr) < 0; });
        }));
    }

    using Ms = std::chrono::duration<double, std::milli>;
    const auto mean = std::accumulate(elapsed.begin(), elapsed.end(), std::chrono::nanoseconds::zero()) /
                      static_cast<long long>(SAMPLES);
    const auto low = *std::ranges::min_element(elapsed);
    const auto high = *std::ranges::max_element(elapsed);
    WARN(
        "CompareRows level sort over " << ROW_COUNT << " rows: mean=" << Ms(mean).count()
                                       << " ms (low=" << Ms(low).count() << ", high=" << Ms(high).count() << ")"
    );

    // `CompareLevel` is heavier per call than `CompareEnum` with a
    // precomputed `EnumDictRank` (one `GetLevelForRow` lookup per side
    // vs. an `EnumValueId`-indexed array read), so the ceiling is
    // bumped to 2000 ms vs. the enum sibling's 1500 ms. A regression
    // past 2000 ms typically means either `LevelRankCache` lookups
    // collapsed back to a per-row `mLevelRankCache` walk inside
    // `GetLevelForRow`, or the canonical-name resolve path stopped
    // hitting the cache.
    CHECK(Ms(low).count() < 2000.0);

    // Sanity: ascending canonical-severity order via `GetLevelForRow`.
    for (size_t i = 1; i < indices.size(); ++i)
    {
        const auto lhs = table.GetLevelForRow(indices[i - 1], 0);
        const auto rhs = table.GetLevelForRow(indices[i], 0);
        if (lhs.has_value() && rhs.has_value())
        {
            REQUIRE(static_cast<uint8_t>(*lhs) <= static_cast<uint8_t>(*rhs));
        }
    }
}
