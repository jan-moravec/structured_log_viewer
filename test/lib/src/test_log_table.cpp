#include "common.hpp"

#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_processing.hpp>
#include <loglib/log_table.hpp>
#include <loglib/streaming_log_sink.hpp>

#include <catch2/catch_all.hpp>

#include <utility>
#include <vector>

using namespace loglib;

TEST_CASE("Initialize a LogTable with given LogData and LogConfigurationManager", "[log_table]")
{
    // Setup test data
    TestLogFile testFile;
    testFile.Write("line1\nline2");
    std::unique_ptr<LogFile> logFile = testFile.CreateLogFile();

    // Create test log lines
    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"key1", std::string("value1")}}, testKeys, LogFileReference(*logFile, 0));
    testLines.emplace_back(LogMap{{"key2", std::string("value2")}}, testKeys, LogFileReference(*logFile, 1));

    LogData logData(std::move(logFile), std::move(testLines), std::move(testKeys));

    // Create test configuration
    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back({"Header1", {"key1"}, "{}", LogConfiguration::Type::any, {}});
    logConfiguration.columns.push_back({"Header2", {"key2"}, "{}", LogConfiguration::Type::any, {}});
    TestLogConfiguration testLogConfiguration;
    testLogConfiguration.Write(logConfiguration);
    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    // Initialize LogTable with the data and configuration
    LogTable logTable(std::move(logData), std::move(manager));

    // Verify LogTable correctly initialized with the provided data and configuration
    REQUIRE(logTable.RowCount() == 2);
    REQUIRE(logTable.ColumnCount() == 2);
    CHECK(logTable.GetHeader(0) == "Header1");
    CHECK(logTable.GetHeader(1) == "Header2");
    CHECK(std::get<std::string>(logTable.GetValue(0, 0)) == "value1");
    CHECK(std::get<std::string>(logTable.GetValue(1, 1)) == "value2");
    REQUIRE(logTable.Data().Files().size() == 1);
    REQUIRE(logTable.Data().Lines().size() == 2);
    CHECK(logTable.Data().Lines()[0].FileReference().GetLineNumber() == 0);
    CHECK(logTable.Data().Lines()[0].FileReference().GetLine() == "line1");
    CHECK(logTable.Data().Lines()[0].FileReference().GetPath() == testFile.GetFilePath());
    CHECK(logTable.Data().Lines()[1].FileReference().GetLineNumber() == 1);
    CHECK(logTable.Data().Lines()[1].FileReference().GetLine() == "line2");
    CHECK(logTable.Data().Lines()[1].FileReference().GetPath() == testFile.GetFilePath());
}

TEST_CASE("Update LogTable with new LogData", "[log_table]")
{
    TestLogFile testFile("log_file.json");
    TestLogFile newTestFile("new_log_file.json");

    // Setup initial test data
    testFile.Write("file1\nfile2");
    std::unique_ptr<LogFile> logFile = testFile.CreateLogFile();

    // Create initial log lines
    KeyIndex initialKeys;
    std::vector<LogLine> initialLines;
    initialLines.emplace_back(LogMap{{"key1", std::string("value1")}}, initialKeys, LogFileReference(*logFile, 0));

    LogData initialData(std::move(logFile), std::move(initialLines), std::move(initialKeys));

    // Create initial configuration
    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back({"Header1", {"key1"}, "{}", LogConfiguration::Type::any, {}});
    TestLogConfiguration testLogConfiguration;
    testLogConfiguration.Write(logConfiguration);
    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    // Initialize LogTable with the initial data and configuration
    LogTable logTable(std::move(initialData), std::move(manager));

    // Verify initial state
    REQUIRE(logTable.RowCount() == 1);
    REQUIRE(logTable.ColumnCount() == 1);
    CHECK(logTable.GetHeader(0) == "Header1");
    CHECK(std::get<std::string>(logTable.GetValue(0, 0)) == "value1");

    // Create new test data to update with
    newTestFile.Write("newfile1\nnewfile2");
    std::unique_ptr<LogFile> newLogFile = newTestFile.CreateLogFile();

    // Create new log lines with new keys
    KeyIndex newKeys;
    std::vector<LogLine> newLines;
    newLines.emplace_back(LogMap{{"key2", std::string("value2")}}, newKeys, LogFileReference(*newLogFile, 0));

    LogData newData(std::move(newLogFile), std::move(newLines), std::move(newKeys));

    // Update the LogTable with the new data
    logTable.Update(std::move(newData));

    // Verify the LogTable was updated correctly
    REQUIRE(logTable.RowCount() == 2);
    REQUIRE(logTable.ColumnCount() == 2); // Configuration should be updated with the new key
    CHECK(logTable.GetHeader(0) == "Header1");
    CHECK(logTable.GetHeader(1) == "key2"); // New column should be added with default header matching key name
    CHECK(std::get<std::string>(logTable.GetValue(0, 0)) == "value1");
    CHECK(std::get<std::string>(logTable.GetValue(1, 1)) == "value2");
    REQUIRE(logTable.Data().Files().size() == 2);
    REQUIRE(logTable.Data().Lines().size() == 2);
    CHECK(logTable.Data().Lines()[0].FileReference().GetLineNumber() == 0);
    CHECK(logTable.Data().Lines()[0].FileReference().GetLine() == "file1");
    CHECK(logTable.Data().Lines()[0].FileReference().GetPath() == testFile.GetFilePath());
    CHECK(logTable.Data().Lines()[1].FileReference().GetLineNumber() == 0);
    CHECK(logTable.Data().Lines()[1].FileReference().GetLine() == "newfile1");
    CHECK(logTable.Data().Lines()[1].FileReference().GetPath() == newTestFile.GetFilePath());
}

namespace
{

// Helper that builds a LogLine bound to @p keys, inserting each (key, value) pair via
// GetOrInsert so the test does not have to spell KeyIds explicitly.
LogLine MakeLine(KeyIndex &keys, LogFile &file, const std::vector<std::pair<std::string, LogValue>> &fields)
{
    std::vector<std::pair<KeyId, LogValue>> sorted;
    sorted.reserve(fields.size());
    for (const auto &[key, value] : fields)
    {
        sorted.emplace_back(keys.GetOrInsert(key), value);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    return LogLine(std::move(sorted), keys, LogFileReference(file, 0));
}

// Helper that snapshots a column → KeyId range from a given KeyIndex into a StreamedBatch::newKeys.
StreamedBatch BuildStreamedBatch(
    KeyIndex &keys,
    LogFile &file,
    const std::vector<std::vector<std::pair<std::string, LogValue>>> &lines,
    size_t prevKeyCount,
    size_t firstLineNumber
)
{
    StreamedBatch batch;
    batch.firstLineNumber = firstLineNumber;
    batch.lines.reserve(lines.size());
    for (const auto &fields : lines)
    {
        batch.lines.push_back(MakeLine(keys, file, fields));
    }
    const size_t currentKeyCount = keys.Size();
    if (currentKeyCount > prevKeyCount)
    {
        batch.newKeys.reserve(currentKeyCount - prevKeyCount);
        for (size_t i = prevKeyCount; i < currentKeyCount; ++i)
        {
            batch.newKeys.emplace_back(std::string(keys.KeyOf(static_cast<KeyId>(i))));
        }
    }
    return batch;
}

} // namespace

TEST_CASE("LogTable::AppendBatch -- steady-state batches with no new keys do not extend columns",
          "[log_table][append_batch]")
{
    TestLogFile testFile("steady.json");
    testFile.Write("");
    auto logFile = std::make_unique<LogFile>(testFile.GetFilePath());
    LogFile *filePtr = logFile.get();

    LogTable table;
    table.BeginStreaming(std::move(logFile));

    KeyIndex &keys = table.Data().Keys();
    auto batchA = BuildStreamedBatch(
        keys, *filePtr, {{{"key1", std::string("v1a")}, {"key2", std::string("v2a")}}}, 0, 1
    );
    table.AppendBatch(std::move(batchA));

    REQUIRE(table.RowCount() == 1);
    REQUIRE(table.ColumnCount() == 2);
    const std::string firstHeader = table.GetHeader(0);
    const std::string secondHeader = table.GetHeader(1);

    auto batchB = BuildStreamedBatch(
        keys, *filePtr, {{{"key1", std::string("v1b")}, {"key2", std::string("v2b")}}}, keys.Size(), 2
    );
    REQUIRE(batchB.newKeys.empty());
    table.AppendBatch(std::move(batchB));

    CHECK(table.RowCount() == 2);
    CHECK(table.ColumnCount() == 2);
    CHECK(table.GetHeader(0) == firstHeader);
    CHECK(table.GetHeader(1) == secondHeader);
    CHECK(!table.LastBackfillRange().has_value());
}

TEST_CASE("LogTable::AppendBatch -- new-key batches append columns at the end",
          "[log_table][append_batch]")
{
    TestLogFile testFile("append_columns.json");
    testFile.Write("");
    auto logFile = std::make_unique<LogFile>(testFile.GetFilePath());
    LogFile *filePtr = logFile.get();

    LogTable table;
    table.BeginStreaming(std::move(logFile));

    KeyIndex &keys = table.Data().Keys();
    table.AppendBatch(BuildStreamedBatch(
        keys, *filePtr, {{{"alpha", std::string("a1")}, {"beta", std::string("b1")}}}, 0, 1
    ));
    REQUIRE(table.ColumnCount() == 2);
    const std::string alphaHeader = table.GetHeader(0);
    const std::string betaHeader = table.GetHeader(1);

    table.AppendBatch(BuildStreamedBatch(
        keys, *filePtr, {{{"alpha", std::string("a2")}, {"gamma", std::string("g1")}}}, keys.Size() - 1, 2
    ));

    REQUIRE(table.ColumnCount() == 3);
    CHECK(table.GetHeader(0) == alphaHeader); // unchanged position
    CHECK(table.GetHeader(1) == betaHeader);  // unchanged position
    CHECK(table.GetHeader(2) == "gamma");     // appended at the end
    CHECK(!table.LastBackfillRange().has_value());
}

TEST_CASE("LogTable::AppendBatch -- empty-rows-only batches do not crash", "[log_table][append_batch]")
{
    TestLogFile testFile("empty_rows.json");
    testFile.Write("");
    auto logFile = std::make_unique<LogFile>(testFile.GetFilePath());

    LogTable table;
    table.BeginStreaming(std::move(logFile));

    StreamedBatch errorOnly;
    errorOnly.firstLineNumber = 1;
    errorOnly.errors.emplace_back("Something went wrong on line 1");
    REQUIRE_NOTHROW(table.AppendBatch(std::move(errorOnly)));

    CHECK(table.RowCount() == 0);
    CHECK(!table.LastBackfillRange().has_value());

    StreamedBatch newKeysOnly;
    newKeysOnly.firstLineNumber = 1;
    newKeysOnly.newKeys.emplace_back("future_key");
    REQUIRE_NOTHROW(table.AppendBatch(std::move(newKeysOnly)));

    CHECK(table.RowCount() == 0);
    CHECK(table.ColumnCount() == 1);
    CHECK(table.GetHeader(0) == "future_key");

    StreamedBatch entirelyEmpty;
    entirelyEmpty.firstLineNumber = 1;
    REQUIRE_NOTHROW(table.AppendBatch(std::move(entirelyEmpty)));
    CHECK(table.RowCount() == 0);
}

// PRD req. 4.1.13 — the column → KeyId cache is append-only. Update() and
// AppendBatch() may grow it but must never reorder existing columns or
// insert new ones in the middle. This is the contract that lets the Qt
// LogModel emit beginInsertColumns / endInsertColumns over the trailing
// range without invalidating any persistent QModelIndex held by the view.
TEST_CASE("LogTable column to KeyId cache is append-only across Update and AppendBatch",
          "[log_table][append_only]")
{
    TestLogFile testFile("append_only.json");
    testFile.Write("");
    auto logFile = std::make_unique<LogFile>(testFile.GetFilePath());
    LogFile *filePtr = logFile.get();

    LogTable table;
    table.BeginStreaming(std::move(logFile));

    KeyIndex &keys = table.Data().Keys();

    // Pin every header position observed so far. After each subsequent
    // Update/AppendBatch the test re-reads the header at each remembered
    // index and asserts it has not moved — that is the invariant we want
    // to lock down end-to-end.
    std::vector<std::string> pinnedHeaders;

    auto pinAndCheck = [&](LogTable &t) {
        for (size_t i = 0; i < pinnedHeaders.size(); ++i)
        {
            REQUIRE(t.ColumnCount() > i);
            CHECK(t.GetHeader(static_cast<int>(i)) == pinnedHeaders[i]);
        }
        pinnedHeaders.clear();
        const size_t total = t.ColumnCount();
        pinnedHeaders.reserve(total);
        for (size_t i = 0; i < total; ++i)
        {
            pinnedHeaders.emplace_back(t.GetHeader(static_cast<int>(i)));
        }
    };

    // Step 1: introduce {alpha, beta} via AppendBatch.
    table.AppendBatch(BuildStreamedBatch(
        keys,
        *filePtr,
        {
            {{"alpha", std::string("a1")}, {"beta", std::string("b1")}},
        },
        0,
        1
    ));
    REQUIRE(table.ColumnCount() == 2);
    pinAndCheck(table);

    // Step 2: AppendBatch with one new key (gamma). It must land at index 2,
    // alpha and beta keep their indices.
    table.AppendBatch(BuildStreamedBatch(
        keys,
        *filePtr,
        {
            {{"alpha", std::string("a2")}, {"gamma", std::string("g1")}},
        },
        keys.Size() - 1,
        2
    ));
    REQUIRE(table.ColumnCount() == 3);
    CHECK(table.GetHeader(2) == "gamma");
    pinAndCheck(table);

    // Step 3: a steady-state batch with NO new keys must not change the
    // column count or any header position.
    table.AppendBatch(BuildStreamedBatch(
        keys,
        *filePtr,
        {
            {{"alpha", std::string("a3")}, {"beta", std::string("b3")}, {"gamma", std::string("g3")}},
        },
        keys.Size(),
        3
    ));
    REQUIRE(table.ColumnCount() == 3);
    pinAndCheck(table);

    // Step 4: introduce two new keys at once via AppendBatch (delta, epsilon).
    // They must appear at the end in the order Stage B observed them, never
    // anywhere else in the existing range. Pre-existing alpha/beta/gamma keep
    // their positions so any consumer that has cached column indices does not
    // need to remap them.
    table.AppendBatch(BuildStreamedBatch(
        keys,
        *filePtr,
        {
            {{"delta", std::string("d1")}, {"epsilon", std::string("e1")}},
        },
        keys.Size(),
        4
    ));
    REQUIRE(table.ColumnCount() == 5);
    CHECK(table.GetHeader(3) == "delta");
    CHECK(table.GetHeader(4) == "epsilon");
    pinAndCheck(table);
}

// Same append-only contract, but driven exclusively through the legacy
// LogTable::Update() path. Update() is also used outside streaming (e.g. when
// MainWindow opens additional files into an already-populated LogTable), so it
// has to honour the same invariant — already-known column indices stable, new
// columns at the end. Note that LogConfigurationManager::Update DOES still
// apply the timestamp auto-promotion swap for the *first* timestamp column
// it sees on a freshly-built configuration; this test deliberately avoids
// timestamp keys so that legacy reorder path stays out of scope.
TEST_CASE("LogTable::Update is append-only for non-timestamp keys", "[log_table][append_only]")
{
    TestLogFile fileA("log_file_initial.json");
    TestLogFile fileB("log_file_second.json");
    TestLogFile fileC("log_file_third.json");

    fileA.Write("a1\n");
    auto logFileA = fileA.CreateLogFile();

    KeyIndex keysA;
    std::vector<LogLine> linesA;
    linesA.emplace_back(LogMap{{"alpha", std::string("a1")}, {"beta", std::string("b1")}}, keysA, LogFileReference(*logFileA, 0));

    LogData dataA(std::move(logFileA), std::move(linesA), std::move(keysA));

    LogConfiguration cfg;
    cfg.columns.push_back({"alpha", {"alpha"}, "{}", LogConfiguration::Type::any, {}});
    cfg.columns.push_back({"beta", {"beta"}, "{}", LogConfiguration::Type::any, {}});
    TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table(std::move(dataA), std::move(mgr));
    REQUIRE(table.ColumnCount() == 2);
    REQUIRE(table.GetHeader(0) == "alpha");
    REQUIRE(table.GetHeader(1) == "beta");

    // Update with a separate LogData that introduces a single new key (gamma).
    // gamma must land at index 2; alpha and beta keep their positions.
    fileB.Write("b1\n");
    auto logFileB = fileB.CreateLogFile();

    KeyIndex keysB;
    std::vector<LogLine> linesB;
    linesB.emplace_back(LogMap{{"alpha", std::string("a2")}, {"gamma", std::string("g1")}}, keysB, LogFileReference(*logFileB, 0));

    LogData dataB(std::move(logFileB), std::move(linesB), std::move(keysB));
    table.Update(std::move(dataB));

    REQUIRE(table.ColumnCount() == 3);
    CHECK(table.GetHeader(0) == "alpha");
    CHECK(table.GetHeader(1) == "beta");
    CHECK(table.GetHeader(2) == "gamma");

    // Update with a third LogData that introduces yet another key (delta).
    // delta lands at index 3; everything else stays put.
    fileC.Write("c1\n");
    auto logFileC = fileC.CreateLogFile();

    KeyIndex keysC;
    std::vector<LogLine> linesC;
    linesC.emplace_back(LogMap{{"beta", std::string("b3")}, {"delta", std::string("d1")}}, keysC, LogFileReference(*logFileC, 0));

    LogData dataC(std::move(logFileC), std::move(linesC), std::move(keysC));
    table.Update(std::move(dataC));

    REQUIRE(table.ColumnCount() == 4);
    CHECK(table.GetHeader(0) == "alpha");
    CHECK(table.GetHeader(1) == "beta");
    CHECK(table.GetHeader(2) == "gamma");
    CHECK(table.GetHeader(3) == "delta");
}

TEST_CASE("LogTable::AppendBatch -- auto-promoted time column triggers back-fill on already-appended rows",
          "[log_table][append_batch]")
{
    InitializeTimezoneData();

    TestLogFile testFile("backfill.json");
    testFile.Write("");
    auto logFile = std::make_unique<LogFile>(testFile.GetFilePath());
    LogFile *filePtr = logFile.get();

    LogTable table;
    table.BeginStreaming(std::move(logFile));

    KeyIndex &keys = table.Data().Keys();
    // Batch 1: no timestamp column yet; rows carry only `msg`.
    table.AppendBatch(BuildStreamedBatch(
        keys,
        *filePtr,
        {
            {{"msg", std::string("first")}},
            {{"msg", std::string("second")}},
        },
        0,
        1
    ));
    REQUIRE(table.RowCount() == 2);
    REQUIRE(table.ColumnCount() == 1);
    CHECK(!table.LastBackfillRange().has_value());

    // Batch 2: introduces `timestamp`. The auto-promotion heuristic in
    // `LogConfigurationManager::AppendKeys` recognises the name and creates a Type::time
    // column at the END of the configuration, so column index 1 should be the new time
    // column. The two already-appended rows did not see Stage B's timestamp pass (their
    // batch did not even contain the key) so AppendBatch must back-fill them — the new row
    // also gets parsed by the same back-fill (because its KeyId is not yet in the snapshot).
    table.AppendBatch(BuildStreamedBatch(
        keys,
        *filePtr,
        {
            {{"msg", std::string("third")}, {"timestamp", std::string("2024-01-15T12:34:56")}},
        },
        keys.Size(),
        3
    ));

    REQUIRE(table.RowCount() == 3);
    REQUIRE(table.ColumnCount() == 2);
    CHECK(table.GetHeader(1) == "timestamp");

    // The back-fill range covers exactly the new time column (index 1, inclusive on both ends).
    REQUIRE(table.LastBackfillRange().has_value());
    CHECK(table.LastBackfillRange()->first == 1);
    CHECK(table.LastBackfillRange()->second == 1);

    // The third row's timestamp is now a parsed TimeStamp (the back-fill ran over all rows
    // including the just-appended one). Rows 0 and 1 stay monostate because they never
    // carried the key.
    const LogValue thirdRowTimestamp = table.GetValue(2, 1);
    CHECK(std::holds_alternative<TimeStamp>(thirdRowTimestamp));

    // A subsequent steady-state batch with no new keys must NOT re-back-fill — the time
    // column's KeyId is now in the snapshot.
    table.AppendBatch(BuildStreamedBatch(
        keys,
        *filePtr,
        {{{"msg", std::string("fourth")}, {"timestamp", std::string("2024-01-15T12:34:57")}}},
        keys.Size(),
        4
    ));
    CHECK(!table.LastBackfillRange().has_value());
}

// PRD task 9.0 / §4.8.2 — `LogTable::AppendBatch::RefreshColumnKeyIds` thrashing
// fix. Before task 9.0 this test (named "RefreshColumnKeyIds Find call count
// baseline") asserted `findCount == kBatches × kKeyCount` to pin the pre-fix
// number of `KeyIndex::Find` calls a wide-configuration steady-state stream
// pays per AppendBatch. After task 9.0 the call is gated on
// `!batch.newKeys.empty()` (and, when keys *do* arrive, restricted to columns
// whose `keys` overlap with `batch.newKeys` via
// `RefreshColumnKeyIdsForKeys`). For pure steady-state batches that means
// `findCount == 0` instead of `kBatches × kKeyCount` — a saved ~99 000 Find
// calls on the GUI thread for the worked example in PRD §4.8.2 (100 columns
// × 1 000 batches × no new keys after batch 1).
TEST_CASE(
    "LogTable::AppendBatch -- RefreshColumnKeyIds skipped on steady-state batches",
    "[log_table][refresh_no_alloc]"
)
{
    constexpr int kKeyCount = 100;
    constexpr int kBatches = 1'000;

    TestLogFile testFile("refresh_no_alloc.json");
    testFile.Write("");
    auto logFile = std::make_unique<LogFile>(testFile.GetFilePath());
    LogFile *filePtr = logFile.get();

    // Build a single column whose `keys` list contains kKeyCount entries.
    // Pre-task-9.0 RefreshColumnKeyIds called `Find` once per (column, key)
    // pair on every AppendBatch invocation; post-fix, an empty `newKeys`
    // means RefreshColumnKeyIdsForKeys returns immediately without touching
    // the KeyIndex at all.
    LogConfiguration cfg;
    LogConfiguration::Column wide;
    wide.header = "Wide";
    wide.printFormat = "{}";
    wide.type = LogConfiguration::Type::any;
    wide.keys.reserve(kKeyCount);
    for (int i = 0; i < kKeyCount; ++i)
    {
        wide.keys.push_back("k" + std::to_string(i));
    }
    cfg.columns.push_back(std::move(wide));

    TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(logFile));

    // Pre-populate every key in the KeyIndex once so each AppendBatch is a
    // pure steady-state batch (no `newKeys`, no auto-promotion).
    KeyIndex &keys = table.Data().Keys();
    for (int i = 0; i < kKeyCount; ++i)
    {
        keys.GetOrInsert("k" + std::to_string(i));
    }

    // Reset counters so other test cases do not leak counts into us. Note:
    // BeginStreaming above runs the *full* RefreshColumnKeyIds (which does
    // pay the kKeyCount Find calls) — but that is expected and not what we
    // are measuring here. We are measuring the per-batch AppendBatch
    // contribution in the steady state.
    KeyIndex::ResetInstrumentationCounters();

    for (int batchIdx = 0; batchIdx < kBatches; ++batchIdx)
    {
        StreamedBatch batch;
        batch.firstLineNumber = static_cast<size_t>(batchIdx + 1);
        batch.lines.push_back(MakeLine(keys, *filePtr, {{"k0", std::string("steady")}}));
        REQUIRE(batch.newKeys.empty());
        table.AppendBatch(std::move(batch));
    }

    // PRD §4.8.2: on steady-state batches with empty `newKeys`,
    // AppendBatch must not call `KeyIndex::Find` at all. No time column
    // means the back-fill loop in AppendBatch contributes zero additional
    // Find calls either, so the total is exactly zero.
    const std::size_t findCount = KeyIndex::LoadFindCount();
    INFO(
        "Find calls = " << findCount << " over " << kBatches << " AppendBatch calls × " << kKeyCount
                        << " keys (expected 0 post-task-9.0)"
    );
    CHECK(findCount == 0);

    // Sanity: the lines actually accumulated and the column count did not
    // grow (no `newKeys` arrived).
    CHECK(table.RowCount() == static_cast<size_t>(kBatches));
    CHECK(table.ColumnCount() == 1);
}

// PRD task 9.3 / §4.8.2 — when `batch.newKeys` *is* non-empty,
// `RefreshColumnKeyIdsForKeys` must only re-walk columns whose `keys` overlap
// with the arrived `newKeys`. Untouched columns keep their cached KeyIds.
//
// Setup: two columns, "Touched" (keys = {"k0"}) and "Untouched" (keys = {"u0",
// "u1", ..., "u49"}). A batch arrives carrying a brand-new key "k0_new" that
// is *also* in the "Touched" column's `keys` list (we add it before calling
// AppendBatch), so RefreshColumnKeyIdsForKeys must walk the 2-key Touched
// column (1 Find call from the lookup loop after `affected == true`) and skip
// the 50-key Untouched column entirely.
TEST_CASE(
    "LogTable::AppendBatch -- RefreshColumnKeyIdsForKeys skips columns without overlap",
    "[log_table][refresh_no_alloc]"
)
{
    constexpr int kUntouchedKeyCount = 50;

    TestLogFile testFile("refresh_no_alloc_incremental.json");
    testFile.Write("");
    auto logFile = std::make_unique<LogFile>(testFile.GetFilePath());
    LogFile *filePtr = logFile.get();

    LogConfiguration cfg;

    LogConfiguration::Column touched;
    touched.header = "Touched";
    touched.printFormat = "{}";
    touched.type = LogConfiguration::Type::any;
    touched.keys = {"k0", "k0_new"};
    cfg.columns.push_back(std::move(touched));

    LogConfiguration::Column untouched;
    untouched.header = "Untouched";
    untouched.printFormat = "{}";
    untouched.type = LogConfiguration::Type::any;
    untouched.keys.reserve(kUntouchedKeyCount);
    for (int i = 0; i < kUntouchedKeyCount; ++i)
    {
        untouched.keys.push_back("u" + std::to_string(i));
    }
    cfg.columns.push_back(std::move(untouched));

    TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(logFile));

    // Pre-populate every key the columns reference except the brand-new one
    // ("k0_new") so BeginStreaming's `RefreshColumnKeyIds` walks them all.
    KeyIndex &keys = table.Data().Keys();
    keys.GetOrInsert("k0");
    for (int i = 0; i < kUntouchedKeyCount; ++i)
    {
        keys.GetOrInsert("u" + std::to_string(i));
    }

    // First batch: announce one brand-new key ("k0_new") that the "Touched"
    // column already references. The "Untouched" column has no overlap, so
    // RefreshColumnKeyIdsForKeys must skip its 50-key Find loop entirely.
    KeyIndex::ResetInstrumentationCounters();

    StreamedBatch batch;
    batch.firstLineNumber = 1;
    batch.newKeys = {"k0_new"};
    batch.lines.push_back(MakeLine(keys, *filePtr, {{"k0_new", std::string("hello")}}));
    table.AppendBatch(std::move(batch));

    // Expected Find calls: exactly `Touched.keys.size() == 2` (one for "k0",
    // one for the freshly-arrived "k0_new"). The Untouched column contributes
    // zero. Pre-task-9.0 we would have paid 2 + 50 = 52 Find calls here.
    const std::size_t findCount = KeyIndex::LoadFindCount();
    INFO(
        "Find calls = " << findCount << " — expected 2 (only the Touched column whose keys overlap with newKeys is refreshed)"
    );
    CHECK(findCount == 2);

    CHECK(table.RowCount() == 1);
    CHECK(table.ColumnCount() == 2);
}
