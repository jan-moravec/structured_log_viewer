#include "common.hpp"

#include <loglib/file_line_source.hpp>
#include <loglib/internal/compact_log_value.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_processing.hpp>
#include <loglib/bytes_producer.hpp>
#include <loglib/log_table.hpp>
#include <loglib/stream_line_source.hpp>
#include <loglib/log_parse_sink.hpp>

#include <catch2/catch_all.hpp>

#include <chrono>
#include <utility>
#include <vector>

using namespace loglib;

TEST_CASE("Initialize a LogTable with given LogData and LogConfigurationManager", "[log_table]")
{
    // Setup test data
    TestLogFile testFile;
    testFile.Write("line1\nline2");
    auto source = testFile.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();

    // Create test log lines
    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"key1", std::string("value1")}}, testKeys, *sourcePtr, 0);
    testLines.emplace_back(LogMap{{"key2", std::string("value2")}}, testKeys, *sourcePtr, 1);

    LogData logData(std::move(source), std::move(testLines), std::move(testKeys));

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
    REQUIRE(logTable.Data().Sources().size() == 1);
    REQUIRE(logTable.Data().Lines().size() == 2);
    const auto &line0 = logTable.Data().Lines()[0];
    const auto &line1 = logTable.Data().Lines()[1];
    REQUIRE(line0.Source() != nullptr);
    REQUIRE(line1.Source() != nullptr);
    CHECK(line0.LineId() == 0);
    CHECK(line0.Source()->RawLine(line0.LineId()) == "line1");
    CHECK(line0.Source()->Path() == testFile.GetFilePath());
    CHECK(line1.LineId() == 1);
    CHECK(line1.Source()->RawLine(line1.LineId()) == "line2");
    CHECK(line1.Source()->Path() == testFile.GetFilePath());
}

TEST_CASE("Update LogTable with new LogData", "[log_table]")
{
    TestLogFile testFile("log_file.json");
    TestLogFile newTestFile("new_log_file.json");

    // Setup initial test data
    testFile.Write("file1\nfile2");
    auto source = testFile.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();

    // Create initial log lines
    KeyIndex initialKeys;
    std::vector<LogLine> initialLines;
    initialLines.emplace_back(LogMap{{"key1", std::string("value1")}}, initialKeys, *sourcePtr, 0);

    LogData initialData(std::move(source), std::move(initialLines), std::move(initialKeys));

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
    auto newSource = newTestFile.CreateFileLineSource();
    FileLineSource *newSourcePtr = newSource.get();

    // Create new log lines with new keys
    KeyIndex newKeys;
    std::vector<LogLine> newLines;
    newLines.emplace_back(LogMap{{"key2", std::string("value2")}}, newKeys, *newSourcePtr, 0);

    LogData newData(std::move(newSource), std::move(newLines), std::move(newKeys));

    // Update the LogTable with the new data
    logTable.Update(std::move(newData));

    // Verify the LogTable was updated correctly
    REQUIRE(logTable.RowCount() == 2);
    REQUIRE(logTable.ColumnCount() == 2); // Configuration should be updated with the new key
    CHECK(logTable.GetHeader(0) == "Header1");
    CHECK(logTable.GetHeader(1) == "key2"); // New column should be added with default header matching key name
    CHECK(std::get<std::string>(logTable.GetValue(0, 0)) == "value1");
    CHECK(std::get<std::string>(logTable.GetValue(1, 1)) == "value2");
    REQUIRE(logTable.Data().Sources().size() == 2);
    REQUIRE(logTable.Data().Lines().size() == 2);
    const auto &line0 = logTable.Data().Lines()[0];
    const auto &line1 = logTable.Data().Lines()[1];
    REQUIRE(line0.Source() != nullptr);
    REQUIRE(line1.Source() != nullptr);
    CHECK(line0.LineId() == 0);
    CHECK(line0.Source()->RawLine(line0.LineId()) == "file1");
    CHECK(line0.Source()->Path() == testFile.GetFilePath());
    CHECK(line1.LineId() == 0);
    CHECK(line1.Source()->RawLine(line1.LineId()) == "newfile1");
    CHECK(line1.Source()->Path() == newTestFile.GetFilePath());
}

TEST_CASE("LogTable::Reset preserves the loaded LogConfiguration", "[log_table]")
{
    // Regression: `Reset()` clears data but must keep the configuration
    // (otherwise `LoadConfiguration → File → Open` would lose column layout).
    TestLogFile testFile;
    testFile.Write("line1\nline2");
    auto source = testFile.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();

    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"key1", std::string("value1")}}, testKeys, *sourcePtr, 0);
    testLines.emplace_back(LogMap{{"key2", std::string("value2")}}, testKeys, *sourcePtr, 1);
    LogData logData(std::move(source), std::move(testLines), std::move(testKeys));

    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back({"CustomA", {"key1"}, "{}", LogConfiguration::Type::any, {}});
    logConfiguration.columns.push_back({"CustomB", {"key2"}, "{}", LogConfiguration::Type::any, {}});
    LogConfiguration::LogFilter filter;
    filter.type = LogConfiguration::LogFilter::Type::string;
    filter.row = 0;
    filter.filterString = "value1";
    filter.matchType = LogConfiguration::LogFilter::Match::contains;
    logConfiguration.filters.push_back(filter);

    TestLogConfiguration testLogConfiguration;
    testLogConfiguration.Write(logConfiguration);
    LogConfigurationManager manager;
    manager.Load(testLogConfiguration.GetFilePath());

    LogTable logTable(std::move(logData), std::move(manager));
    REQUIRE(logTable.RowCount() == 2);
    REQUIRE(logTable.ColumnCount() == 2);

    logTable.Reset();

    CHECK(logTable.RowCount() == 0);
    REQUIRE(logTable.ColumnCount() == 2);
    CHECK(logTable.GetHeader(0) == "CustomA");
    CHECK(logTable.GetHeader(1) == "CustomB");
    REQUIRE(logTable.Configuration().Configuration().filters.size() == 1);
    CHECK(logTable.Configuration().Configuration().filters.front().filterString.value_or("") == "value1");
}

namespace
{

// Helper that builds a LogLine bound to @p keys, inserting each (key, value) pair via
// GetOrInsert so the test does not have to spell KeyIds explicitly.
LogLine MakeLine(KeyIndex &keys, LineSource &source, const std::vector<std::pair<std::string, LogValue>> &fields)
{
    std::vector<std::pair<KeyId, LogValue>> sorted;
    sorted.reserve(fields.size());
    for (const auto &[key, value] : fields)
    {
        sorted.emplace_back(keys.GetOrInsert(key), value);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    return LogLine(std::move(sorted), keys, source, 0);
}

// Helper that snapshots a column → KeyId range from a given KeyIndex into a StreamedBatch::newKeys.
StreamedBatch BuildStreamedBatch(
    KeyIndex &keys,
    LineSource &source,
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
        batch.lines.push_back(MakeLine(keys, source, fields));
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

TEST_CASE(
    "LogTable::AppendBatch -- steady-state batches with no new keys do not extend columns", "[log_table][append_batch]"
)
{
    TestLogFile testFile("steady.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    auto batchA =
        BuildStreamedBatch(keys, *sourcePtr, {{{"key1", std::string("v1a")}, {"key2", std::string("v2a")}}}, 0, 1);
    table.AppendBatch(std::move(batchA));

    REQUIRE(table.RowCount() == 1);
    REQUIRE(table.ColumnCount() == 2);
    const std::string firstHeader = table.GetHeader(0);
    const std::string secondHeader = table.GetHeader(1);

    auto batchB = BuildStreamedBatch(
        keys, *sourcePtr, {{{"key1", std::string("v1b")}, {"key2", std::string("v2b")}}}, keys.Size(), 2
    );
    REQUIRE(batchB.newKeys.empty());
    table.AppendBatch(std::move(batchB));

    CHECK(table.RowCount() == 2);
    CHECK(table.ColumnCount() == 2);
    CHECK(table.GetHeader(0) == firstHeader);
    CHECK(table.GetHeader(1) == secondHeader);
    CHECK(!table.LastBackfillRange().has_value());
}

TEST_CASE("LogTable::AppendBatch -- new-key batches append columns at the end", "[log_table][append_batch]")
{
    TestLogFile testFile("append_columns.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    table.AppendBatch(
        BuildStreamedBatch(keys, *sourcePtr, {{{"alpha", std::string("a1")}, {"beta", std::string("b1")}}}, 0, 1)
    );
    REQUIRE(table.ColumnCount() == 2);
    const std::string alphaHeader = table.GetHeader(0);
    const std::string betaHeader = table.GetHeader(1);

    table.AppendBatch(BuildStreamedBatch(
        keys, *sourcePtr, {{{"alpha", std::string("a2")}, {"gamma", std::string("g1")}}}, keys.Size() - 1, 2
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
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));

    LogTable table;
    table.BeginStreaming(std::move(source));

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

// The column → KeyId cache is append-only: `Update()` and `AppendBatch()`
// may grow it but must never reorder existing columns or insert new ones in
// the middle. This is the contract that lets the Qt `LogModel` emit
// `beginInsertColumns` / `endInsertColumns` over the trailing range without
// invalidating any persistent `QModelIndex` held by the view.
TEST_CASE("LogTable column to KeyId cache is append-only across Update and AppendBatch", "[log_table][append_only]")
{
    TestLogFile testFile("append_only.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();

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

    // {alpha, beta}.
    table.AppendBatch(BuildStreamedBatch(
        keys,
        *sourcePtr,
        {
            {{"alpha", std::string("a1")}, {"beta", std::string("b1")}},
        },
        0,
        1
    ));
    REQUIRE(table.ColumnCount() == 2);
    pinAndCheck(table);

    // +gamma at index 2; alpha/beta stay put.
    table.AppendBatch(BuildStreamedBatch(
        keys,
        *sourcePtr,
        {
            {{"alpha", std::string("a2")}, {"gamma", std::string("g1")}},
        },
        keys.Size() - 1,
        2
    ));
    REQUIRE(table.ColumnCount() == 3);
    CHECK(table.GetHeader(2) == "gamma");
    pinAndCheck(table);

    // No-new-keys batch leaves column count and positions unchanged.
    table.AppendBatch(BuildStreamedBatch(
        keys,
        *sourcePtr,
        {
            {{"alpha", std::string("a3")}, {"beta", std::string("b3")}, {"gamma", std::string("g3")}},
        },
        keys.Size(),
        3
    ));
    REQUIRE(table.ColumnCount() == 3);
    pinAndCheck(table);

    // Two new keys land at the end in observation order; existing positions stable.
    table.AppendBatch(BuildStreamedBatch(
        keys,
        *sourcePtr,
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

// Same append-only contract via the legacy `LogTable::Update()` path
// (used when opening additional files into an already-populated LogTable).
// Avoids timestamp keys so the auto-promotion reorder stays out of scope.
TEST_CASE("LogTable::Update is append-only for non-timestamp keys", "[log_table][append_only]")
{
    TestLogFile fileA("log_file_initial.json");
    TestLogFile fileB("log_file_second.json");
    TestLogFile fileC("log_file_third.json");

    fileA.Write("a1\n");
    auto sourceA = fileA.CreateFileLineSource();
    FileLineSource *sourceAPtr = sourceA.get();

    KeyIndex keysA;
    std::vector<LogLine> linesA;
    linesA.emplace_back(
        LogMap{{"alpha", std::string("a1")}, {"beta", std::string("b1")}}, keysA, *sourceAPtr, 0
    );

    LogData dataA(std::move(sourceA), std::move(linesA), std::move(keysA));

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
    auto sourceB = fileB.CreateFileLineSource();
    FileLineSource *sourceBPtr = sourceB.get();

    KeyIndex keysB;
    std::vector<LogLine> linesB;
    linesB.emplace_back(
        LogMap{{"alpha", std::string("a2")}, {"gamma", std::string("g1")}}, keysB, *sourceBPtr, 0
    );

    LogData dataB(std::move(sourceB), std::move(linesB), std::move(keysB));
    table.Update(std::move(dataB));

    REQUIRE(table.ColumnCount() == 3);
    CHECK(table.GetHeader(0) == "alpha");
    CHECK(table.GetHeader(1) == "beta");
    CHECK(table.GetHeader(2) == "gamma");

    // Update with a third LogData that introduces yet another key (delta).
    // delta lands at index 3; everything else stays put.
    fileC.Write("c1\n");
    auto sourceC = fileC.CreateFileLineSource();
    FileLineSource *sourceCPtr = sourceC.get();

    KeyIndex keysC;
    std::vector<LogLine> linesC;
    linesC.emplace_back(
        LogMap{{"beta", std::string("b3")}, {"delta", std::string("d1")}}, keysC, *sourceCPtr, 0
    );

    LogData dataC(std::move(sourceC), std::move(linesC), std::move(keysC));
    table.Update(std::move(dataC));

    REQUIRE(table.ColumnCount() == 4);
    CHECK(table.GetHeader(0) == "alpha");
    CHECK(table.GetHeader(1) == "beta");
    CHECK(table.GetHeader(2) == "gamma");
    CHECK(table.GetHeader(3) == "delta");
}

TEST_CASE(
    "LogTable::AppendBatch -- auto-promoted time column triggers back-fill on already-appended rows",
    "[log_table][append_batch]"
)
{
    InitializeTimezoneData();

    TestLogFile testFile("backfill.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    // Batch 1: no timestamp column yet; rows carry only `msg`.
    table.AppendBatch(BuildStreamedBatch(
        keys,
        *sourcePtr,
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
        *sourcePtr,
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

    // A subsequent steady-state batch with no new keys must NOT re-back-fill *existing*
    // rows — the column has already been bridged. But the running parser's Stage B was
    // never told about the post-snapshot column (its `LogConfiguration` snapshot pre-dates
    // it), so the new row's timestamp is still a raw string when it arrives. AppendBatch
    // must therefore back-fill the slice of just-appended rows; otherwise rows past the
    // first batch keep raw string timestamps. Because the slice is exactly the rows about
    // to fire `beginInsertRows` in `LogModel::AppendBatch`, we deliberately do *not* set
    // `mLastBackfillRange` (which would emit a redundant `dataChanged` over rows that are
    // simultaneously being inserted into the view).
    table.AppendBatch(BuildStreamedBatch(
        keys,
        *sourcePtr,
        {{{"msg", std::string("fourth")}, {"timestamp", std::string("2024-01-15T12:34:57")}}},
        keys.Size(),
        4
    ));
    CHECK(!table.LastBackfillRange().has_value());
    const LogValue fourthRowTimestamp = table.GetValue(3, 1);
    CHECK(std::holds_alternative<TimeStamp>(fourthRowTimestamp));

    // A second post-snapshot batch with multiple new rows confirms the slice back-fill
    // keeps running on every batch (not just the immediate batch after first observation).
    table.AppendBatch(BuildStreamedBatch(
        keys,
        *sourcePtr,
        {
            {{"msg", std::string("fifth")}, {"timestamp", std::string("2024-01-15T12:34:58")}},
            {{"msg", std::string("sixth")}, {"timestamp", std::string("2024-01-15T12:34:59")}},
        },
        keys.Size(),
        5
    ));
    CHECK(!table.LastBackfillRange().has_value());
    CHECK(std::holds_alternative<TimeStamp>(table.GetValue(4, 1)));
    CHECK(std::holds_alternative<TimeStamp>(table.GetValue(5, 1)));
}

// Mirrors the JSON parser flow: the configuration handed to the parser
// AppendBatch must recognise Stage-B-handled time columns (via
// `mStageBSnapshotTimeKeys`) and skip `BackfillTimestampColumn`; otherwise
// it walks every batch and allocates discarded `fmt::format` error strings.
TEST_CASE(
    "LogTable::AppendBatch -- Stage B inline promotion of configured time columns is not re-back-filled",
    "[log_table][append_batch][snapshot_time_keys]"
)
{
    InitializeTimezoneData();

    TestLogFile testFile("snapshot_time_keys.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back({"timestamp", {"timestamp"}, "%F %H:%M:%S", LogConfiguration::Type::time, {"%FT%T", "%F %T"}}
    );
    cfg.columns.push_back({"msg", {"msg"}, "{}", LogConfiguration::Type::any, {}});
    TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));

    // After `BeginStreaming`, the time-column key must already be resolvable
    // (mirroring `BuildTimeColumnSpecs`) — without this the snapshot set
    // would be permanently empty and the bug below would not be detected.
    KeyIndex &keys = table.Keys();
    const KeyId timestampId = keys.Find("timestamp");
    REQUIRE(timestampId != kInvalidKeyId);

    // Build a batch the way Stage C would: lines already carry promoted
    // `TimeStamp` values; "timestamp" is not in `newKeys` (pre-registered
    // before `prevKeyCount` was captured), but a brand-new "msg" key is.
    const TimeStamp ts = std::chrono::time_point_cast<std::chrono::microseconds>(
        std::chrono::sys_days{std::chrono::year{2024} / std::chrono::January / 15} + std::chrono::hours{12} +
        std::chrono::minutes{34} + std::chrono::seconds{56}
    );

    StreamedBatch batch = BuildStreamedBatch(
        keys,
        *sourcePtr,
        {
            {{"timestamp", ts}, {"msg", std::string("hello")}},
            {{"timestamp", ts}, {"msg", std::string("world")}},
        },
        keys.Size(),
        1
    );
    REQUIRE(batch.newKeys.size() == 1);
    CHECK(batch.newKeys.front() == "msg");

    table.AppendBatch(std::move(batch));

    REQUIRE(table.RowCount() == 2);
    REQUIRE(table.ColumnCount() == 2);
    CHECK(table.GetHeader(0) == "timestamp");
    CHECK(table.GetHeader(1) == "msg");

    // The time column was Stage-B-handled, so `AppendBatch` must not run a
    // back-fill — a non-empty range here means redundant `dataChanged` plus
    // discarded per-line `fmt::format` error strings on the GUI thread.
    CHECK(!table.LastBackfillRange().has_value());

    // Inline-promoted values survive intact and remain accessible through
    // the column->KeyId cache the GUI uses.
    CHECK(std::holds_alternative<TimeStamp>(table.GetValue(0, 0)));
    CHECK(std::get<TimeStamp>(table.GetValue(0, 0)) == ts);
    CHECK(std::get<std::string>(table.GetValue(0, 1)) == "hello");

    // A subsequent steady-state batch must also leave the time column alone:
    // the slice back-fill path is just as wasteful as the first-observation
    // path when Stage B already did the work.
    StreamedBatch batch2 = BuildStreamedBatch(
        keys,
        *sourcePtr,
        {
            {{"timestamp", ts}, {"msg", std::string("again")}},
        },
        keys.Size(),
        3
    );
    REQUIRE(batch2.newKeys.empty());
    table.AppendBatch(std::move(batch2));

    CHECK(table.RowCount() == 3);
    CHECK(!table.LastBackfillRange().has_value());
    CHECK(std::holds_alternative<TimeStamp>(table.GetValue(2, 0)));
}

// AppendBatch gates `RefreshColumnKeyIds` on `!batch.newKeys.empty()`;
// steady-state batches must not call `Find` at all.
TEST_CASE(
    "LogTable::AppendBatch -- RefreshColumnKeyIds skipped on steady-state batches", "[log_table][refresh_no_alloc]"
)
{
    constexpr int kKeyCount = 100;
    constexpr int kBatches = 1'000;

    TestLogFile testFile("refresh_no_alloc.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    // Single column with kKeyCount keys; an empty `newKeys` must short-circuit
    // before any `Find` call.
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
    table.BeginStreaming(std::move(source));

    // Pre-populate every key in the KeyIndex once so each AppendBatch is a
    // pure steady-state batch (no `newKeys`, no auto-promotion).
    KeyIndex &keys = table.Keys();
    for (int i = 0; i < kKeyCount; ++i)
    {
        static_cast<void>(keys.GetOrInsert("k" + std::to_string(i)));
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
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"k0", std::string("steady")}}));
        REQUIRE(batch.newKeys.empty());
        table.AppendBatch(std::move(batch));
    }

    // On steady-state batches with empty `newKeys`, `AppendBatch` must not
    // call `KeyIndex::Find` at all. No time column means the back-fill
    // loop contributes zero additional `Find` calls either, so the total
    // is exactly zero.
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

// When `batch.newKeys` *is* non-empty, `RefreshColumnKeyIdsForKeys` must
// only re-walk columns whose `keys` overlap with the arrived `newKeys`.
// Untouched columns keep their cached KeyIds.
//
// Setup: two columns, "Touched" (keys = {"k0"}) and "Untouched" (keys = {"u0",
// "u1", ..., "u49"}). A batch arrives carrying a brand-new key "k0_new" that
// is *also* in the "Touched" column's `keys` list (we add it before calling
// AppendBatch), so RefreshColumnKeyIdsForKeys must walk the 2-key Touched
// column (1 Find call from the lookup loop after `affected == true`) and skip
// the 50-key Untouched column entirely.
TEST_CASE(
    "LogTable::AppendBatch -- RefreshColumnKeyIdsForKeys skips columns without overlap", "[log_table][refresh_no_alloc]"
)
{
    constexpr int kUntouchedKeyCount = 50;

    TestLogFile testFile("refresh_no_alloc_incremental.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

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
    table.BeginStreaming(std::move(source));

    // Pre-populate every key the columns reference except the brand-new one
    // ("k0_new") so BeginStreaming's `RefreshColumnKeyIds` walks them all.
    KeyIndex &keys = table.Keys();
    static_cast<void>(keys.GetOrInsert("k0"));
    for (int i = 0; i < kUntouchedKeyCount; ++i)
    {
        static_cast<void>(keys.GetOrInsert("u" + std::to_string(i)));
    }

    // First batch: announce one brand-new key ("k0_new") that the "Touched"
    // column already references. The "Untouched" column has no overlap, so
    // RefreshColumnKeyIdsForKeys must skip its 50-key Find loop entirely.
    KeyIndex::ResetInstrumentationCounters();

    StreamedBatch batch;
    batch.firstLineNumber = 1;
    batch.newKeys = {"k0_new"};
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"k0_new", std::string("hello")}}));
    table.AppendBatch(std::move(batch));

    // Expected Find calls: exactly `Touched.keys.size() == 2` (one for "k0",
    // one for the freshly-arrived "k0_new"). The Untouched column contributes
    // zero. Pre-task-9.0 we would have paid 2 + 50 = 52 Find calls here.
    const std::size_t findCount = KeyIndex::LoadFindCount();
    INFO(
        "Find calls = " << findCount
                        << " — expected 2 (only the Touched column whose keys overlap with newKeys is refreshed)"
    );
    CHECK(findCount == 2);

    CHECK(table.RowCount() == 1);
    CHECK(table.ColumnCount() == 2);
}

namespace
{

/// Build a `StreamedBatch` of @p count synthetic `LogLine` rows
/// referencing @p streamSource and numbered
/// `[firstLineId, firstLineId + count)`, each carrying a single
/// `value` field equal to its line id. The first batch in a sequence
/// should set @p declareNewKey so `LogTable::PreviewAppend`'s column
/// predictor matches reality. Mirrors the helper used by the Qt-side
/// retention tests in `main_window_test.cpp` so the library-level cases
/// here stay symmetric with the GUI ones.
///
/// Each row is also published into @p streamSource via `AppendLine`
/// so `LineSource::RawLine` round-trips. Tests expect the source's
/// next-line id to track @p firstLineId.
StreamedBatch MakeStreamBatch(
    StreamLineSource &streamSource,
    KeyIndex &keys,
    KeyId valueKey,
    size_t firstLineId,
    size_t count,
    bool declareNewKey
)
{
    StreamedBatch batch;
    batch.firstLineNumber = firstLineId;
    if (declareNewKey)
    {
        batch.newKeys.emplace_back(std::string("value"));
    }
    batch.lines.reserve(count);
    for (size_t i = 0; i < count; ++i)
    {
        const size_t lineId = firstLineId + i;
        const size_t publishedId = streamSource.AppendLine("raw" + std::to_string(lineId), std::string{});
        REQUIRE(publishedId == lineId);

        std::vector<std::pair<KeyId, internal::CompactLogValue>> compactValues;
        compactValues.emplace_back(valueKey, internal::CompactLogValue::MakeInt64(static_cast<int64_t>(lineId)));
        batch.lines.emplace_back(std::move(compactValues), keys, streamSource, lineId);
    }
    return batch;
}

} // namespace

// `LogTable::EvictPrefixRows` is the library-level primitive `LogModel`'s FIFO
// retention machinery is built on. The Qt-side tests in
// `test/app/src/main_window_test.cpp` cover the begin/endRemoveRows wiring;
// here we drive the row-vector mechanics directly so a regression in the
// vector-erase path surfaces in the library test suite without needing the
// Qt offscreen platform.
//
// Coverage:
//   1. cap = 1000, feed 5000 stream lines via 50 batches of 100 lines each.
//      After every breach the table is trimmed back to the cap; the surviving
//      rows correspond to the most-recent (5000 - 1000 + 1) = 4001 .. 5000
//      LineIds (FIFO drops oldest).
//   2. Giant batch: a single 2000-line batch into a 1000-cap table. Once the
//      head of the batch is collapsed (the same trim the GUI side performs in
//      `LogModel::AppendBatch`) and `EvictPrefixRows` runs, the surviving
//      rows are LineIds 1001 .. 2000.
TEST_CASE("LogTable::EvictPrefixRows trims oldest stream rows in source order", "[log_table][retention]")
{
    auto streamSource = std::make_unique<StreamLineSource>(std::filesystem::path("<test>"), nullptr);
    StreamLineSource &streamSourceRef = *streamSource;
    LogTable table;
    table.BeginStreaming(std::move(streamSource));

    KeyIndex &keys = table.Keys();
    const KeyId valueKey = keys.GetOrInsert(std::string("value"));

    constexpr size_t kCap = 1000;
    constexpr size_t kBatchSize = 100;
    constexpr size_t kTotalLines = 5000;
    static_assert(kTotalLines % kBatchSize == 0, "kBatchSize must evenly divide kTotalLines");

    for (size_t batchStart = 0; batchStart < kTotalLines; batchStart += kBatchSize)
    {
        const bool declareNewKey = (batchStart == 0);
        table.AppendBatch(MakeStreamBatch(streamSourceRef, keys, valueKey, batchStart + 1, kBatchSize, declareNewKey));

        // Mirror `LogModel::AppendBatch`'s post-append trim: any rows past
        // the cap are evicted in source order. Cap is checked *after*
        // append because PreviewAppend would also be valid here, but the
        // post-append form keeps the test self-contained.
        if (table.RowCount() > kCap)
        {
            table.EvictPrefixRows(table.RowCount() - kCap);
        }
        REQUIRE(table.RowCount() <= kCap);
    }

    REQUIRE(table.RowCount() == kCap);
    // The first surviving row's `value` is `kTotalLines - kCap + 1 = 4001`;
    // the last is `kTotalLines = 5000`.
    const auto firstValue = std::get<int64_t>(table.GetValue(0, 0));
    const auto lastValue = std::get<int64_t>(table.GetValue(kCap - 1, 0));
    CHECK(firstValue == static_cast<int64_t>(kTotalLines - kCap + 1));
    CHECK(lastValue == static_cast<int64_t>(kTotalLines));
}

TEST_CASE("LogTable::EvictPrefixRows handles a giant single-batch overflow", "[log_table][retention]")
{
    auto streamSource = std::make_unique<StreamLineSource>(std::filesystem::path("<test>"), nullptr);
    StreamLineSource &streamSourceRef = *streamSource;
    LogTable table;
    table.BeginStreaming(std::move(streamSource));

    KeyIndex &keys = table.Keys();
    const KeyId valueKey = keys.GetOrInsert(std::string("value"));

    constexpr size_t kCap = 1000;
    constexpr size_t kGiantBatch = 2000;

    // Mirrors the GUI-side "giant-batch collapse": the head
    // of the batch is dropped before it lands in `LogTable::AppendBatch`, so
    // the visible model never breaches the cap. The synthesized rows are
    // numbered to match what the GUI side would have surfaced after head
    // drop; the source's first AppendLine bumps `mNextLineId` past the
    // head-dropped range so the published id matches.
    const size_t headDrop = kGiantBatch - kCap;
    for (size_t i = 0; i < headDrop; ++i)
    {
        streamSourceRef.AppendLine("dropped" + std::to_string(i + 1), std::string{});
    }
    streamSourceRef.EvictBefore(headDrop + 1);
    table.AppendBatch(
        MakeStreamBatch(streamSourceRef, keys, valueKey, headDrop + 1, kCap, /*declareNewKey=*/true)
    );

    REQUIRE(table.RowCount() == kCap);
    CHECK(std::get<int64_t>(table.GetValue(0, 0)) == static_cast<int64_t>(headDrop + 1));
    CHECK(std::get<int64_t>(table.GetValue(kCap - 1, 0)) == static_cast<int64_t>(kGiantBatch));
}

TEST_CASE(
    "LogTable::EvictPrefixRows is a no-op for count == 0 and clears for count >= RowCount", "[log_table][retention]"
)
{
    LogTable table;
    auto streamSource = std::make_unique<StreamLineSource>(std::filesystem::path("synthetic"), nullptr);
    StreamLineSource &streamSourceRef = *streamSource;
    table.BeginStreaming(std::move(streamSource));

    KeyIndex &keys = table.Keys();
    const KeyId valueKey = keys.GetOrInsert(std::string("value"));

    table.AppendBatch(MakeStreamBatch(streamSourceRef, keys, valueKey, 1, 5, /*declareNewKey=*/true));
    REQUIRE(table.RowCount() == 5);

    // count == 0: documented well-defined behaviour (no rows removed).
    table.EvictPrefixRows(0);
    REQUIRE(table.RowCount() == 5);
    CHECK(std::get<int64_t>(table.GetValue(0, 0)) == int64_t{1});

    // count == RowCount: documented to clear every row in source order.
    table.EvictPrefixRows(5);
    CHECK(table.RowCount() == 0);

    // count > RowCount: same outcome as count == RowCount (saturating clear).
    // After the prior `EvictPrefixRows(5)` the source has cleared lines 1..5,
    // so its next-to-publish lineId is 6 — match it here so `MakeStreamBatch`'s
    // `AppendLine`-publishes-monotonic invariant holds.
    table.AppendBatch(MakeStreamBatch(streamSourceRef, keys, valueKey, 6, 3, /*declareNewKey=*/false));
    REQUIRE(table.RowCount() == 3);
    table.EvictPrefixRows(99);
    CHECK(table.RowCount() == 0);
}

// `AppendStreaming` is the multi-file streaming-append entry point used
// by the GUI's sequential file-queue open flow. It must (i) install the
// new source without resetting `mData`, (ii) reuse the existing
// `KeyIndex` so columns line up across files, and (iii) make per-batch
// line offsets land in the *new* source via `LogData::BackFileSource()`.
TEST_CASE(
    "LogTable::AppendStreaming -- multi-file accumulation preserves rows and routes offsets to the back source",
    "[log_table][append_streaming]"
)
{
    // Declare *both* `TestLogFile`s above `LogTable`: their dtors run
    // `std::filesystem::remove` (throwing variant); on Windows you cannot
    // delete an mmap'd file, so they must outlive the table that holds
    // the mmap.
    TestLogFile fileA("multifile_a.json");
    fileA.Write("alpha\nbeta\n");
    TestLogFile fileB("multifile_b.json");
    fileB.Write("gamma\ndelta\n");

    auto sourceA = std::make_unique<FileLineSource>(std::make_unique<LogFile>(fileA.GetFilePath()));
    FileLineSource *sourceAPtr = sourceA.get();

    LogTable table;
    table.BeginStreaming(std::move(sourceA));
    KeyIndex &keys = table.Keys();

    // First file's batch: one row, two new keys. We don't push any
    // `localLineOffsets` here -- this test focuses on `AppendStreaming`'s
    // routing of the *next* batch's offsets, not on first-file accounting.
    StreamedBatch batchA;
    batchA.firstLineNumber = 1;
    batchA.lines.push_back(MakeLine(keys, *sourceAPtr, {{"key1", std::string("a1")}, {"key2", std::string("a2")}}));
    batchA.newKeys.emplace_back("key1");
    batchA.newKeys.emplace_back("key2");
    table.AppendBatch(std::move(batchA));

    const KeyId k1 = keys.Find(std::string("key1"));
    const KeyId k2 = keys.Find(std::string("key2"));
    REQUIRE(k1 != kInvalidKeyId);
    REQUIRE(k2 != kInvalidKeyId);

    REQUIRE(table.RowCount() == 1);
    REQUIRE(table.Data().Sources().size() == 1);
    REQUIRE(table.Data().FrontFileSource() == sourceAPtr);
    REQUIRE(table.Data().BackFileSource() == sourceAPtr);

    const size_t fileAOffsetCountBefore = sourceAPtr->File().GetLineCount();

    // Append a second file. Existing rows / KeyIndex / column cache must
    // survive.
    auto sourceB = std::make_unique<FileLineSource>(std::make_unique<LogFile>(fileB.GetFilePath()));
    FileLineSource *sourceBPtr = sourceB.get();

    table.AppendStreaming(std::move(sourceB));

    CHECK(table.RowCount() == 1);
    REQUIRE(table.Data().Sources().size() == 2);
    CHECK(table.Data().FrontFileSource() == sourceAPtr);
    CHECK(table.Data().BackFileSource() == sourceBPtr);

    // KeyIndex survived -- the same KeyIds reused below.
    CHECK(keys.Find(std::string("key1")) == k1);
    CHECK(keys.Find(std::string("key2")) == k2);

    // Second file's batch: one row using the existing keys (no newKeys)
    // plus a per-line offset that must land in fileB, not fileA.
    StreamedBatch batchB;
    batchB.firstLineNumber = 1;
    batchB.lines.push_back(MakeLine(keys, *sourceBPtr, {{"key1", std::string("b1")}, {"key2", std::string("b2")}}));
    // The `LogFile` ctor seeds `mLineOffsets = [0]`, so any strictly
    // greater offset suffices to test routing.
    batchB.localLineOffsets = {6};
    table.AppendBatch(std::move(batchB));

    CHECK(table.RowCount() == 2);
    CHECK(table.ColumnCount() == 2);

    // The new offset landed in fileB, leaving fileA's offset table
    // untouched.
    CHECK(sourceAPtr->File().GetLineCount() == fileAOffsetCountBefore);
    CHECK(sourceBPtr->File().GetLineCount() == 1);

    // Cross-file row resolution: row 0 came from fileA, row 1 from fileB.
    const auto &line0 = table.Data().Lines()[0];
    const auto &line1 = table.Data().Lines()[1];
    CHECK(line0.Source() == sourceAPtr);
    CHECK(line1.Source() == sourceBPtr);
}
