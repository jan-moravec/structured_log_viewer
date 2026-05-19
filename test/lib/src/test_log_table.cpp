#include "common.hpp"

#include <loglib/bytes_producer.hpp>
#include <loglib/enum_dictionary.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/internal/compact_log_value.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_level.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_parse_sink.hpp>
#include <loglib/log_processing.hpp>
#include <loglib/log_table.hpp>
#include <loglib/stream_line_source.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <chrono>
#include <utility>
#include <vector>

using namespace loglib;

TEST_CASE("Initialize a LogTable with given LogData and LogConfigurationManager", "[log_table]")
{
    // Setup test data
    const TestLogFile testFile;
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
    logConfiguration.columns.push_back(
        {.header = "Header1",
         .keys = {"key1"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Any,
         .parseFormats = {}}
    );
    logConfiguration.columns.push_back(
        {.header = "Header2",
         .keys = {"key2"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Any,
         .parseFormats = {}}
    );
    const TestLogConfiguration testLogConfiguration;
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
    const TestLogFile testFile("log_file.json");
    const TestLogFile newTestFile("new_log_file.json");

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
    logConfiguration.columns.push_back(
        {.header = "Header1",
         .keys = {"key1"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Any,
         .parseFormats = {}}
    );
    const TestLogConfiguration testLogConfiguration;
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
    const TestLogFile testFile;
    testFile.Write("line1\nline2");
    auto source = testFile.CreateFileLineSource();
    FileLineSource *sourcePtr = source.get();

    KeyIndex testKeys;
    std::vector<LogLine> testLines;
    testLines.emplace_back(LogMap{{"key1", std::string("value1")}}, testKeys, *sourcePtr, 0);
    testLines.emplace_back(LogMap{{"key2", std::string("value2")}}, testKeys, *sourcePtr, 1);
    LogData logData(std::move(source), std::move(testLines), std::move(testKeys));

    LogConfiguration logConfiguration;
    logConfiguration.columns.push_back(
        {.header = "CustomA",
         .keys = {"key1"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Any,
         .parseFormats = {}}
    );
    logConfiguration.columns.push_back(
        {.header = "CustomB",
         .keys = {"key2"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Any,
         .parseFormats = {}}
    );
    LogConfiguration::LogFilter filter;
    filter.type = LogConfiguration::LogFilter::Type::String;
    filter.row = 0;
    filter.filterString = "value1";
    filter.matchType = LogConfiguration::LogFilter::Match::Contains;
    logConfiguration.filters.push_back(filter);

    const TestLogConfiguration testLogConfiguration;
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
    std::ranges::sort(sorted, [](const auto &a, const auto &b) { return a.first < b.first; });
    return {std::move(sorted), keys, source, 0};
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
            batch.newKeys.emplace_back(keys.KeyOf(static_cast<KeyId>(i)));
        }
    }
    return batch;
}

} // namespace

TEST_CASE(
    "LogTable::AppendBatch -- steady-state batches with no new keys do not extend columns", "[log_table][append_batch]"
)
{
    const TestLogFile testFile("steady.json");
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
    // No `LastBackfillRange` check: stream-mode auto-detection promotes both
    // columns to enum after row 2, legitimately reporting a back-fill.
}

TEST_CASE("LogTable::AppendBatch -- new-key batches append columns at the end", "[log_table][append_batch]")
{
    const TestLogFile testFile("append_columns.json");
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
    // No `LastBackfillRange` check: `alpha` promotes to enum at row 2 and
    // legitimately back-fills. Column ordering is what we guard.
}

TEST_CASE("LogTable::AppendBatch -- empty-rows-only batches do not crash", "[log_table][append_batch]")
{
    const TestLogFile testFile("empty_rows.json");
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
    const TestLogFile testFile("append_only.json");
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
            CHECK(t.GetHeader(i) == pinnedHeaders[i]);
        }
        pinnedHeaders.clear();
        const size_t total = t.ColumnCount();
        pinnedHeaders.reserve(total);
        for (size_t i = 0; i < total; ++i)
        {
            pinnedHeaders.emplace_back(t.GetHeader(i));
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
    const TestLogFile fileA("log_file_initial.json");
    const TestLogFile fileB("log_file_second.json");
    const TestLogFile fileC("log_file_third.json");

    fileA.Write("a1\n");
    auto sourceA = fileA.CreateFileLineSource();
    FileLineSource *sourceAPtr = sourceA.get();

    KeyIndex keysA;
    std::vector<LogLine> linesA;
    linesA.emplace_back(LogMap{{"alpha", std::string("a1")}, {"beta", std::string("b1")}}, keysA, *sourceAPtr, 0);

    LogData dataA(std::move(sourceA), std::move(linesA), std::move(keysA));

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "alpha",
         .keys = {"alpha"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Any,
         .parseFormats = {}}
    );
    cfg.columns.push_back(
        {.header = "beta",
         .keys = {"beta"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Any,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
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
    linesB.emplace_back(LogMap{{"alpha", std::string("a2")}, {"gamma", std::string("g1")}}, keysB, *sourceBPtr, 0);

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
    linesC.emplace_back(LogMap{{"beta", std::string("b3")}, {"delta", std::string("d1")}}, keysC, *sourceCPtr, 0);

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

    const TestLogFile testFile("backfill.json");
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
    // `msg` promotes to enum during this batch and legitimately back-fills
    // column 0; the timestamp back-fill in batch 2 is what this test checks.

    // Batch 2: introduces `timestamp`. The auto-promotion heuristic in
    // `LogConfigurationManager::AppendKeys` recognises the name and creates a Type::Time
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
    const auto backfill = table.LastBackfillRange();
    REQUIRE(backfill.has_value());
    if (backfill.has_value())
    {
        CHECK(backfill->first == 1);
        CHECK(backfill->second == 1);
    }

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

    const TestLogFile testFile("snapshot_time_keys.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "timestamp",
         .keys = {"timestamp"},
         .printFormat = "%F %H:%M:%S",
         .type = LogConfiguration::Type::Time,
         .parseFormats = {"%FT%T", "%F %T"}}
    );
    cfg.columns.push_back(
        {.header = "msg",
         .keys = {"msg"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Any,
         .parseFormats = {},
         .visible = true,
         .levelMapping = {},
         .autoDetect = false}
    );
    const TestLogConfiguration cfgFile;
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
    REQUIRE(timestampId != INVALID_KEY_ID);

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
    constexpr int KEY_COUNT = 100;
    constexpr int BATCHES = 1'000;

    const TestLogFile testFile("refresh_no_alloc.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    // Single column with KEY_COUNT keys; an empty `newKeys` must short-circuit
    // before any `Find` call.
    LogConfiguration cfg;
    LogConfiguration::Column wide;
    wide.header = "Wide";
    wide.printFormat = "{}";
    wide.type = LogConfiguration::Type::Any;
    wide.keys.reserve(KEY_COUNT);
    for (int i = 0; i < KEY_COUNT; ++i)
    {
        wide.keys.push_back("k" + std::to_string(i));
    }
    cfg.columns.push_back(std::move(wide));

    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));

    // Pre-populate every key in the KeyIndex once so each AppendBatch is a
    // pure steady-state batch (no `newKeys`, no auto-promotion).
    KeyIndex &keys = table.Keys();
    for (int i = 0; i < KEY_COUNT; ++i)
    {
        static_cast<void>(keys.GetOrInsert("k" + std::to_string(i)));
    }

    // Reset counters so other test cases do not leak counts into us. Note:
    // BeginStreaming above runs the *full* RefreshColumnKeyIds (which does
    // pay the KEY_COUNT Find calls) — but that is expected and not what we
    // are measuring here. We are measuring the per-batch AppendBatch
    // contribution in the steady state.
    KeyIndex::ResetInstrumentationCounters();

    for (int batchIdx = 0; batchIdx < BATCHES; ++batchIdx)
    {
        StreamedBatch batch;
        batch.firstLineNumber = static_cast<size_t>(static_cast<size_t>(batchIdx) + 1u);
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
        "Find calls = " << findCount << " over " << BATCHES << " AppendBatch calls × " << KEY_COUNT
                        << " keys (expected 0 post-task-9.0)"
    );
    CHECK(findCount == 0);

    // Sanity: the lines actually accumulated and the column count did not
    // grow (no `newKeys` arrived).
    CHECK(table.RowCount() == static_cast<size_t>(BATCHES));
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
    constexpr int UNTOUCHED_KEY_COUNT = 50;

    const TestLogFile testFile("refresh_no_alloc_incremental.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;

    LogConfiguration::Column touched;
    touched.header = "Touched";
    touched.printFormat = "{}";
    touched.type = LogConfiguration::Type::Any;
    touched.keys = {"k0", "k0_new"};
    cfg.columns.push_back(std::move(touched));

    LogConfiguration::Column untouched;
    untouched.header = "Untouched";
    untouched.printFormat = "{}";
    untouched.type = LogConfiguration::Type::Any;
    untouched.keys.reserve(UNTOUCHED_KEY_COUNT);
    for (int i = 0; i < UNTOUCHED_KEY_COUNT; ++i)
    {
        untouched.keys.push_back("u" + std::to_string(i));
    }
    cfg.columns.push_back(std::move(untouched));

    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));

    // Pre-populate every key the columns reference except the brand-new one
    // ("k0_new") so BeginStreaming's `RefreshColumnKeyIds` walks them all.
    KeyIndex &keys = table.Keys();
    static_cast<void>(keys.GetOrInsert("k0"));
    for (int i = 0; i < UNTOUCHED_KEY_COUNT; ++i)
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
    StreamLineSource &streamSource, KeyIndex &keys, KeyId valueKey, size_t firstLineId, size_t count, bool declareNewKey
)
{
    StreamedBatch batch;
    batch.firstLineNumber = firstLineId;
    if (declareNewKey)
    {
        batch.newKeys.emplace_back("value");
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

    constexpr size_t CAP = 1000;
    constexpr size_t BATCH_SIZE = 100;
    constexpr size_t TOTAL_LINES = 5000;
    static_assert(TOTAL_LINES % BATCH_SIZE == 0, "BATCH_SIZE must evenly divide TOTAL_LINES");

    for (size_t batchStart = 0; batchStart < TOTAL_LINES; batchStart += BATCH_SIZE)
    {
        const bool declareNewKey = (batchStart == 0);
        table.AppendBatch(MakeStreamBatch(streamSourceRef, keys, valueKey, batchStart + 1, BATCH_SIZE, declareNewKey));

        // Mirror `LogModel::AppendBatch`'s post-append trim: any rows past
        // the cap are evicted in source order. Cap is checked *after*
        // append because PreviewAppend would also be valid here, but the
        // post-append form keeps the test self-contained.
        if (table.RowCount() > CAP)
        {
            table.EvictPrefixRows(table.RowCount() - CAP);
        }
        REQUIRE(table.RowCount() <= CAP);
    }

    REQUIRE(table.RowCount() == CAP);
    // The first surviving row's `value` is `TOTAL_LINES - CAP + 1 = 4001`;
    // the last is `TOTAL_LINES = 5000`.
    const auto firstValue = std::get<int64_t>(table.GetValue(0, 0));
    const auto lastValue = std::get<int64_t>(table.GetValue(CAP - 1, 0));
    CHECK(std::cmp_equal(firstValue, TOTAL_LINES - CAP + 1));
    CHECK(std::cmp_equal(lastValue, TOTAL_LINES));
}

TEST_CASE("LogTable::EvictPrefixRows handles a giant single-batch overflow", "[log_table][retention]")
{
    auto streamSource = std::make_unique<StreamLineSource>(std::filesystem::path("<test>"), nullptr);
    StreamLineSource &streamSourceRef = *streamSource;
    LogTable table;
    table.BeginStreaming(std::move(streamSource));

    KeyIndex &keys = table.Keys();
    const KeyId valueKey = keys.GetOrInsert(std::string("value"));

    constexpr size_t CAP = 1000;
    constexpr size_t GIANT_BATCH = 2000;

    // Mirrors the GUI-side "giant-batch collapse": the head
    // of the batch is dropped before it lands in `LogTable::AppendBatch`, so
    // the visible model never breaches the cap. The synthesized rows are
    // numbered to match what the GUI side would have surfaced after head
    // drop; the source's first AppendLine bumps `mNextLineId` past the
    // head-dropped range so the published id matches.
    const size_t headDrop = GIANT_BATCH - CAP;
    for (size_t i = 0; i < headDrop; ++i)
    {
        streamSourceRef.AppendLine("dropped" + std::to_string(i + 1), std::string{});
    }
    streamSourceRef.EvictBefore(headDrop + 1);
    table.AppendBatch(MakeStreamBatch(streamSourceRef, keys, valueKey, headDrop + 1, CAP, /*declareNewKey=*/true));

    REQUIRE(table.RowCount() == CAP);
    CHECK(std::cmp_equal(std::get<int64_t>(table.GetValue(0, 0)), headDrop + 1));
    CHECK(std::cmp_equal(std::get<int64_t>(table.GetValue(CAP - 1, 0)), GIANT_BATCH));
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
    const TestLogFile fileA("multifile_a.json");
    fileA.Write("alpha\nbeta\n");
    const TestLogFile fileB("multifile_b.json");
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
    REQUIRE(k1 != INVALID_KEY_ID);
    REQUIRE(k2 != INVALID_KEY_ID);

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

namespace
{

/// Append `enoughRows` rows alternating between `values` so the per-column
/// tracker crosses the promotion threshold.
StreamedBatch BuildEnumBatch(
    KeyIndex &keys,
    LineSource &source,
    const std::string &columnKey,
    const std::vector<std::string> &values,
    size_t firstLineNumber,
    size_t rowCount,
    bool announceNewKey
)
{
    std::vector<std::vector<std::pair<std::string, LogValue>>> rows;
    rows.reserve(rowCount);
    for (size_t i = 0; i < rowCount; ++i)
    {
        rows.push_back({{columnKey, std::string(values[i % values.size()])}});
    }
    StreamedBatch batch = BuildStreamedBatch(keys, source, rows, announceNewKey ? 0 : keys.Size(), firstLineNumber);
    if (!announceNewKey)
    {
        batch.newKeys.clear();
    }
    return batch;
}

} // namespace

TEST_CASE(
    "LogTable::AppendBatch -- stream-mode auto-promotion encodes existing rows as DictRef",
    "[log_table][append_batch][enum][stream_mode]"
)
{
    // Streaming promotes at `STREAM_PROMOTION_MIN_ROWS = 2`; the dictionary
    // cap and length cap still bound false positives.
    const TestLogFile testFile("enum_promote.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    const std::string columnName = "tier";
    const std::vector<std::string> tiers = {"alpha", "beta", "gamma", "delta"};

    // Stream-mode threshold = 2: promote and back-fill every prior row.
    constexpr size_t FIRST_BATCH_ROWS = 8;
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, columnName, tiers, 1, FIRST_BATCH_ROWS, true));
    REQUIRE(table.Configuration().Configuration().columns.size() == 1);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);

    const KeyId tierKey = keys.Find(columnName);
    REQUIRE(tierKey != INVALID_KEY_ID);
    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        CHECK(table.Data().Lines()[row].IsDictRef(tierKey));
    }

    // Subsequent batches encode incrementally; no back-fill or type change.
    constexpr size_t SECOND_BATCH_ROWS = 16;
    table.AppendBatch(
        BuildEnumBatch(keys, *sourcePtr, columnName, tiers, FIRST_BATCH_ROWS + 1, SECOND_BATCH_ROWS, false)
    );

    REQUIRE(table.RowCount() == FIRST_BATCH_ROWS + SECOND_BATCH_ROWS);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);
    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        CHECK(table.Data().Lines()[row].IsDictRef(tierKey));
    }
    const EnumDictionary *dict = table.EnumDictionaries().Find(tierKey);
    REQUIRE(dict != nullptr);
    CHECK(dict->Size() == tiers.size());

    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        const LogValue v = table.GetValue(row, 0);
        const auto sv = AsStringView(v);
        REQUIRE(sv.has_value());
        CHECK(*sv == tiers[row % tiers.size()]);
    }
}

TEST_CASE(
    "LogTable::AppendBatch -- enum auto-detection observes long (>SSO) string values without dangling reads",
    "[log_table][append_batch][enum][regression]"
)
{
    // Regression: `RunEnumPassForAppendBatch` used to free the backing
    // `LogValue` before `Observe` ran, dangling non-SSO `OwnedString`
    // payloads. Long values reach the heap so ASan catches a regression.
    const TestLogFile testFile("enum_promote_long_strings.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    // Each value is past the SSO threshold so the bytes live on the heap.
    const std::vector<std::string> levels = {
        "long-component-name-alpha-22",
        "long-component-name-beta-23",
        "long-component-name-gamma-24",
        "long-component-name-delta-25",
    };
    for (const auto &v : levels)
    {
        REQUIRE(v.size() > 15);
    }

    // Drive the tracker past the promotion threshold to exercise every
    // pre-promotion `Observe` call.
    constexpr size_t ROWS = 320;
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "level", levels, 1, ROWS, true));

    REQUIRE(table.RowCount() == ROWS);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);

    const KeyId levelKey = keys.Find("level");
    REQUIRE(levelKey != INVALID_KEY_ID);
    const EnumDictionary *dict = table.EnumDictionaries().Find(levelKey);
    REQUIRE(dict != nullptr);
    CHECK(dict->Size() == levels.size());

    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        const LogValue v = table.GetValue(row, 0);
        const auto sv = AsStringView(v);
        REQUIRE(sv.has_value());
        CHECK(*sv == levels[row % levels.size()]);
    }
}

TEST_CASE(
    "LogTable::AppendBatch -- pre-configured Type::Enumeration column encodes incoming rows without re-walking",
    "[log_table][append_batch][enum]"
)
{
    // Non-level key (`category`) keeps the column Enumeration. A
    // `level`-named column would flip to Level here, which is not
    // what this test is exercising.
    const TestLogFile testFile("enum_preconfigured.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "category",
         .keys = {"category"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Enumeration,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "category", {"info", "warn"}, 1, 4, true));

    REQUIRE(table.RowCount() == 4);
    REQUIRE(table.ColumnCount() == 1);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);

    const KeyId categoryKey = keys.Find("category");
    REQUIRE(categoryKey != INVALID_KEY_ID);

    // Every row encodes on first arrival; no whole-table back-fill since
    // the column was already configured.
    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        CHECK(table.Data().Lines()[row].IsDictRef(categoryKey));
    }
    const EnumDictionary *dict = table.EnumDictionaries().Find(categoryKey);
    REQUIRE(dict != nullptr);
    CHECK(dict->Size() == 2);

    CHECK(!table.LastBackfillRange().has_value());
}

TEST_CASE(
    "LogTable::AppendBatch -- (cap+1)th distinct value demotes enum column to string", "[log_table][append_batch][enum]"
)
{
    const TestLogFile testFile("enum_demote.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "tag",
         .keys = {"tag"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Enumeration,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    // Small cap pins the boundary behaviour without bulk-filling.
    constexpr uint16_t TEST_CAP = 16;
    table.SetEnumValueCap(TEST_CAP);
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();

    std::vector<std::string> capValues;
    capValues.reserve(TEST_CAP);
    for (uint16_t i = 0; i < TEST_CAP; ++i)
    {
        capValues.emplace_back("tag" + std::to_string(i));
    }
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "tag", capValues, 1, TEST_CAP, true));

    const KeyId tagKey = keys.Find("tag");
    REQUIRE(tagKey != INVALID_KEY_ID);
    REQUIRE(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);
    REQUIRE(table.EnumDictionaries().Contains(tagKey));
    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        CHECK(table.Data().Lines()[row].IsDictRef(tagKey));
    }

    // (cap+1)th distinct value forces demotion: prior `DictRef`s become
    // `OwnedString` and the column flips to the terminal `Type::String`.
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "tag", {"never-seen"}, TEST_CAP + 1, 1, false));

    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::String);
    CHECK(!table.EnumDictionaries().Contains(tagKey));
    REQUIRE(table.RowCount() == TEST_CAP + 1);

    for (uint16_t i = 0; i < TEST_CAP; ++i)
    {
        const LogValue v = table.GetValue(i, 0);
        const auto sv = AsStringView(v);
        REQUIRE(sv.has_value());
        CHECK(*sv == capValues[i]);
        CHECK_FALSE(table.Data().Lines()[i].IsDictRef(tagKey));
    }
    const LogValue lastValue = table.GetValue(TEST_CAP, 0);
    REQUIRE(AsStringView(lastValue).has_value());
    CHECK(*AsStringView(lastValue) == "never-seen");
    CHECK_FALSE(table.Data().Lines()[TEST_CAP].IsDictRef(tagKey));

    const auto backfillRangeOpt = table.LastBackfillRange();
    REQUIRE(backfillRangeOpt.has_value());
    if (backfillRangeOpt.has_value())
    {
        CHECK(backfillRangeOpt->first == 0);
        CHECK(backfillRangeOpt->second == 0);
    }
}

TEST_CASE(
    "LogTable -- Type::Any column loaded from saved configuration is locked from auto-promotion",
    "[log_table][append_batch][enum][lock_any][regression]"
)
{
    // A loaded `Type::Any` is terminal and never re-promotes, even if
    // the data shape looks enum-like.
    const TestLogFile testFile("enum_locked_any.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "level",
         .keys = {"level"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Any,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    // Plenty of rows + low cardinality would normally promote, but the
    // user-locked `Type::Any` is terminal and skipped by the auto-detector.
    constexpr size_t ROWS = 320;
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "level", {"info", "warn", "error", "debug"}, 1, ROWS, false));

    REQUIRE(table.RowCount() == ROWS);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Any);
    const KeyId levelKey = keys.Find("level");
    REQUIRE(levelKey != INVALID_KEY_ID);
    CHECK_FALSE(table.EnumDictionaries().Contains(levelKey));
}

TEST_CASE(
    "LogTable -- multi-key column kill state is keyed on the configured canonical key, not the first-resolved KeyId",
    "[log_table][append_batch][enum][multi_key][kill_once][regression]"
)
{
    // Regression: a multi-key column killed via its alias used to
    // re-promote once the canonical key arrived in a later batch.
    const TestLogFile testFile("enum_multi_key_kill_stable.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "level",
         // "level" is canonical; "severity" is the alias.
         .keys = {"level", "severity"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Enumeration,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    constexpr uint16_t TEST_CAP = 4;
    table.SetEnumValueCap(TEST_CAP);
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();

    // Batch 1: alias-only with `cap + 1` distinct values forces demotion.
    std::vector<std::string> overflowValues;
    overflowValues.reserve(TEST_CAP + 1);
    for (uint16_t i = 0; i < TEST_CAP + 1; ++i)
    {
        overflowValues.emplace_back("sev-" + std::to_string(i));
    }
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "severity", overflowValues, 1, TEST_CAP + 1, true));
    REQUIRE(table.Configuration().Configuration().columns.size() == 1);
    // Demote routes to terminal `Type::String`, blocking re-promotion.
    REQUIRE(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::String);

    // Batch 2: canonical key `level` arrives with cap-friendly data.
    // Terminal `Type::String` blocks re-promotion.
    constexpr size_t MIN_ROWS_FOR_PROMOTION = 4096;
    const std::vector<std::string> fewLevelValues = {"info", "warn"};
    table.AppendBatch(BuildEnumBatch(
        keys, *sourcePtr, "level", fewLevelValues, TEST_CAP + 2, MIN_ROWS_FOR_PROMOTION + 1, /*announceNewKey=*/true
    ));

    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::String);
    const KeyId levelKey = keys.Find("level");
    REQUIRE(levelKey != INVALID_KEY_ID);
    CHECK_FALSE(table.EnumDictionaries().Contains(levelKey));
}

TEST_CASE("LogTable::Reset wipes the enum dictionary and trackers", "[log_table][reset][enum]")
{
    // Non-level key (`category`) so `Reset` doesn't have to walk the
    // extra `Type::Level` transition; the contract under test is
    // "configured type survives `Reset`".
    const TestLogFile testFile("enum_reset.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "category",
         .keys = {"category"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Enumeration,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "category", {"info", "warn"}, 1, 4, true));
    REQUIRE_FALSE(table.EnumDictionaries().Empty());
    {
        const KeyId categoryKid = table.Keys().Find("category");
        REQUIRE(categoryKid != INVALID_KEY_ID);
        const EnumDictionary *dict = table.EnumDictionaries().Find(categoryKid);
        REQUIRE(dict != nullptr);
        REQUIRE(dict->Size() > 0);
    }

    table.Reset();
    // Reset re-seeds empty dictionary slots for every configured
    // enum / level column, so the registry isn't strictly empty --
    // each slot just has zero observed values until the next batch.
    CHECK(table.RowCount() == 0);
    REQUIRE(table.Configuration().Configuration().columns.size() == 1);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);
    {
        const KeyId categoryKid = table.Keys().Find("category");
        REQUIRE(categoryKid != INVALID_KEY_ID);
        const EnumDictionary *dict = table.EnumDictionaries().Find(categoryKid);
        REQUIRE(dict != nullptr);
        CHECK(dict->Empty());
    }
}

TEST_CASE(
    "LogTable -- a column killed by overflow stays Type::String across subsequent batches",
    "[log_table][append_batch][enum][kill_once]"
)
{
    // A tracker overflow flips the column to `Type::String`; the type itself
    // blocks future re-promotion via `IsEnumPassEligible`.
    const TestLogFile testFile("enum_kill_once.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    constexpr uint16_t TEST_CAP = 8;
    table.SetEnumValueCap(TEST_CAP);
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();

    // Batch 1: cap+1 distinct values past the row threshold kills the
    // tracker before promotion (many unique values trip the dict-cap kill).
    constexpr size_t MIN_ROWS_FOR_PROMOTION = 4096;
    std::vector<std::string> manyValues;
    manyValues.reserve(TEST_CAP + 1);
    for (uint16_t i = 0; i < TEST_CAP + 1; ++i)
    {
        manyValues.emplace_back("level-" + std::to_string(i));
    }
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "level", manyValues, 1, MIN_ROWS_FOR_PROMOTION, true));
    REQUIRE(table.Configuration().Configuration().columns.size() == 1);
    REQUIRE(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::String);

    // Batch 2: cap-friendly data must NOT re-promote.
    std::vector<std::string> fewValues;
    fewValues.reserve(TEST_CAP);
    for (uint16_t i = 0; i < TEST_CAP; ++i)
    {
        fewValues.emplace_back("level-" + std::to_string(i));
    }
    table.AppendBatch(
        BuildEnumBatch(keys, *sourcePtr, "level", fewValues, MIN_ROWS_FOR_PROMOTION + 1, MIN_ROWS_FOR_PROMOTION, false)
    );
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::String);

    const KeyId levelKey = keys.Find("level");
    REQUIRE(levelKey != INVALID_KEY_ID);
    CHECK_FALSE(table.EnumDictionaries().Contains(levelKey));
}

TEST_CASE(
    "LogTable -- multi-key enum column emits exactly one DictRef slot per row",
    "[log_table][append_batch][enum][multi_key]"
)
{
    // Aliased keys share a dictionary; a row populating both ends up with
    // a single `DictRef` slot.
    const TestLogFile testFile("enum_multi_key.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "severity",
         .keys = {"level", "severity"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Enumeration,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();
    StreamedBatch batch;
    batch.firstLineNumber = 1;
    batch.newKeys = {"level", "severity"};
    batch.lines.push_back(
        MakeLine(keys, *sourcePtr, {{"level", std::string("info")}, {"severity", std::string("warn")}})
    );
    table.AppendBatch(std::move(batch));

    REQUIRE(table.RowCount() == 1);
    const KeyId levelKey = keys.Find("level");
    const KeyId severityKey = keys.Find("severity");
    REQUIRE(levelKey != INVALID_KEY_ID);
    REQUIRE(severityKey != INVALID_KEY_ID);

    // Exactly one slot becomes `DictRef`; the encode loop breaks on the first match.
    const auto &line = table.Data().Lines()[0];
    const bool levelDict = line.IsDictRef(levelKey);
    const bool severityDict = line.IsDictRef(severityKey);
    CHECK((levelDict ^ severityDict));

    // Only the encoded value enters the dictionary.
    const EnumDictionary *dict = table.EnumDictionaries().Find(levelKey);
    REQUIRE(dict != nullptr);
    REQUIRE(dict == table.EnumDictionaries().Find(severityKey));
    CHECK(dict->Size() == 1);
}

TEST_CASE("LogTable::Update -- snapshot-enum keys are seeded against the merged KeyIndex", "[log_table][update][enum]")
{
    // `Update(LogData&&)` must refresh the enum-key snapshot so configured
    // enum columns survive the merge.
    const TestLogFile testFile("enum_update.json");
    testFile.Write("");
    auto sourceA = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourceAPtr = sourceA.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "level",
         .keys = {"level"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Enumeration,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));

    KeyIndex keysA;
    std::vector<LogLine> linesA;
    linesA.push_back(MakeLine(keysA, *sourceAPtr, {{"level", std::string("info")}}));
    linesA.push_back(MakeLine(keysA, *sourceAPtr, {{"level", std::string("warn")}}));
    linesA.push_back(MakeLine(keysA, *sourceAPtr, {{"level", std::string("info")}}));
    linesA.push_back(MakeLine(keysA, *sourceAPtr, {{"level", std::string("error")}}));
    LogData dataA(std::move(sourceA), std::move(linesA), std::move(keysA));
    dataA.MarkTimestampsParsed();

    table.Update(std::move(dataA));

    REQUIRE(table.RowCount() == 4);
    const KeyId levelKey = table.Keys().Find("level");
    REQUIRE(levelKey != INVALID_KEY_ID);
    REQUIRE(table.EnumDictionaries().Contains(levelKey));
    const EnumDictionary *dict = table.EnumDictionaries().Find(levelKey);
    REQUIRE(dict != nullptr);
    CHECK(dict->Size() == 3);
    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        CHECK(table.Data().Lines()[row].IsDictRef(levelKey));
    }
}

TEST_CASE("LogTable::GetEnumValueId returns the dict id for DictRef slots", "[log_table][enum][get_value]")
{
    const TestLogFile testFile("enum_get_value.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "level",
         .keys = {"level"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Enumeration,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "level", {"info", "warn", "error"}, 1, 6, true));

    REQUIRE(table.RowCount() == 6);
    const KeyId levelKey = keys.Find("level");
    REQUIRE(levelKey != INVALID_KEY_ID);
    const EnumDictionary *dict = table.EnumDictionaries().Find(levelKey);
    REQUIRE(dict != nullptr);

    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        const auto vid = table.GetEnumValueId(row, 0);
        REQUIRE(vid.has_value());
        const auto sv = AsStringView(table.GetValue(row, 0));
        REQUIRE(sv.has_value());
        CHECK(dict->Resolve(*vid) == *sv);
    }

    CHECK_FALSE(table.GetEnumValueId(table.RowCount(), 0).has_value());
    CHECK_FALSE(table.GetEnumValueId(0, table.ColumnCount()).has_value());
}

TEST_CASE("LogTable::GetEnumValueId returns nullopt for OwnedString slots", "[log_table][enum][get_value]")
{
    // Pin the column to `Type::Any` with `autoDetect = false` so the
    // auto-detector skips it; pre-promotion / post-demote slots stay
    // `OwnedString` for the filter's string-set fallback path.
    const TestLogFile testFile("enum_get_value_owned.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "channel",
         .keys = {"channel"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Any,
         .parseFormats = {},
         .visible = true,
         .levelMapping = {},
         .autoDetect = false}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();
    // `announceNewKey=true` refreshes the column's KeyId cache after the
    // first batch (the column itself was loaded, not freshly added).
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "channel", {"alpha"}, 1, 4, true));

    REQUIRE(table.RowCount() == 4);
    REQUIRE(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Any);
    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        CHECK_FALSE(table.GetEnumValueId(row, 0).has_value());
    }
}

TEST_CASE(
    "LogTable -- auto-discovered column tolerates a single stray long value",
    "[log_table][append_batch][enum][length_cap]"
)
{
    // Length-cap policy is percentile-based: a single overlong line in an
    // otherwise enum-shaped column stays under the 1% tolerance and the
    // column still promotes.
    const TestLogFile testFile("enum_length_cap.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    // 1/320 = 0.3% long values: well below the 1% tolerance.
    const std::string longValue(80, 'x');
    std::vector<std::string> values;
    values.reserve(320);
    values.push_back(longValue);
    for (size_t i = 1; i < 320; ++i)
    {
        values.emplace_back((i % 2) == 0 ? "alpha" : "beta");
    }
    StreamedBatch batch;
    batch.firstLineNumber = 1;
    batch.newKeys = {"tier"};
    for (const std::string &value : values)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"tier", LogValue{value}}}));
    }
    table.AppendBatch(std::move(batch));

    REQUIRE(table.RowCount() == 320);
    // Column promoted; the lone long line stays as `OwnedString`
    // (under the 1% health tolerance).
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);
    const KeyId tierKey = keys.Find("tier");
    REQUIRE(tierKey != INVALID_KEY_ID);
    REQUIRE(table.EnumDictionaries().Contains(tierKey));
    const EnumDictionary *dict = table.EnumDictionaries().Find(tierKey);
    REQUIRE(dict != nullptr);
    // Dictionary holds {alpha, beta}; the long value never entered.
    CHECK(dict->Size() == 2);
}

TEST_CASE(
    "LogTable -- auto-discovered column demotes when long values exceed the percentile tolerance",
    "[log_table][append_batch][enum][length_cap]"
)
{
    // Inverse of the previous test: 10% long values blow the 1% tolerance
    // once the 50-sample min is satisfied; column flips to `Type::String`.
    const TestLogFile testFile("enum_length_cap_demote.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    const std::string longValue(80, 'x');
    std::vector<std::string> values;
    values.reserve(200);
    for (size_t i = 0; i < 200; ++i)
    {
        // Every 10th row is over-cap.
        values.push_back((i % 10 == 0) ? longValue : "alpha");
    }
    StreamedBatch batch;
    batch.firstLineNumber = 1;
    batch.newKeys = {"tier"};
    for (const std::string &value : values)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"tier", LogValue{value}}}));
    }
    table.AppendBatch(std::move(batch));

    REQUIRE(table.RowCount() == 200);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::String);
    const KeyId tierKey = keys.Find("tier");
    REQUIRE(tierKey != INVALID_KEY_ID);
    CHECK_FALSE(table.EnumDictionaries().Contains(tierKey));
}

TEST_CASE(
    "LogTable -- user-pinned enum column applies the same percentile length-cap policy",
    "[log_table][append_batch][enum][length_cap][user_pinned]"
)
{
    // User-pinned columns share the same tolerance. With 4 rows we are
    // under the 50-sample min, so the column stays `Type::Enumeration`
    // even though 50% of slots are over-cap; long values stay as
    // `OwnedString` and never enter the dictionary.
    const TestLogFile testFile("enum_length_cap_pinned.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "tag",
         .keys = {"tag"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Enumeration,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    const std::string longValue(80, 'q');
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "tag", {longValue, "short"}, 1, 4, true));

    // Below the min-sample threshold: column stays.
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);
    const KeyId tagKey = keys.Find("tag");
    REQUIRE(tagKey != INVALID_KEY_ID);
    REQUIRE(table.EnumDictionaries().Contains(tagKey));
    const EnumDictionary *dict = table.EnumDictionaries().Find(tagKey);
    REQUIRE(dict != nullptr);
    // Only "short" enters the dictionary; the long value stays as
    // `OwnedString` and counts against `longValueSlots`.
    CHECK(dict->Size() == 1);
}

TEST_CASE(
    "LogTable -- user-pinned enum column demotes once long-value tolerance is exceeded across batches",
    "[log_table][append_batch][enum][length_cap][user_pinned]"
)
{
    // Cumulative health check: a user-pinned column's `EnumColumnHealth`
    // accumulates across batches. Once the sample size is large enough
    // and the over-cap fraction breaks 1%, the column demotes to
    // `Type::String` (no more "pinned forever" escape hatch).
    const TestLogFile testFile("enum_length_cap_pinned_demote.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "tag",
         .keys = {"tag"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Enumeration,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    const std::string longValue(80, 'q');
    std::vector<std::string> values;
    values.reserve(100);
    for (size_t i = 0; i < 100; ++i)
    {
        // 10% long values: above the 1% tolerance, past the 50-sample min.
        values.push_back((i % 10 == 0) ? longValue : "short");
    }
    StreamedBatch batch;
    batch.firstLineNumber = 1;
    batch.newKeys = {"tag"};
    for (const std::string &value : values)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"tag", LogValue{value}}}));
    }
    table.AppendBatch(std::move(batch));

    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::String);
    const KeyId tagKey = keys.Find("tag");
    REQUIRE(tagKey != INVALID_KEY_ID);
    CHECK_FALSE(table.EnumDictionaries().Contains(tagKey));
    // Demote fires the back-fill notification.
    REQUIRE(table.LastBackfillRange().has_value());
}

TEST_CASE(
    "LogTable -- sparse stream column with leading missing rows still promotes",
    "[log_table][append_batch][enum][stream_mode][sparse]"
)
{
    // Regression: leading missing rows used to trip the no-string bail
    // and route the column to `Type::Any` permanently. `presenceCount`
    // now gates that bail and the candidate stays alive across batches.
    const TestLogFile testFile("enum_sparse_stream.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    // Pre-register both keys so `feature` lands as a candidate
    // (`Type::Any + autoDetect = true`) column on the first batch,
    // even though no row in batch 1 carries it.
    static_cast<void>(keys.GetOrInsert("unrelated"));
    static_cast<void>(keys.GetOrInsert("feature"));

    // Batch 1: 6 rows where `feature` is absent. The candidate scan walks
    // the first `scanCap = 4` rows, sees no presence, and resets
    // `rowsObserved` instead of routing to a terminal type.
    StreamedBatch batch1;
    batch1.firstLineNumber = 1;
    batch1.newKeys = {"unrelated", "feature"};
    for (size_t i = 0; i < 6; ++i)
    {
        batch1.lines.push_back(MakeLine(keys, *sourcePtr, {{"unrelated", LogValue{static_cast<int64_t>(i)}}}));
    }
    table.AppendBatch(std::move(batch1));

    {
        const auto &columns = table.Configuration().Configuration().columns;
        const auto featureCol = std::ranges::find_if(columns, [](const auto &c) { return c.header == "feature"; });
        REQUIRE(featureCol != columns.end());
        // Stays a candidate -- no premature route to a terminal type.
        CHECK(featureCol->type == LogConfiguration::Type::Any);
        CHECK(featureCol->autoDetect);
    }

    // Batch 2: the column finally appears; the candidate scan sees two
    // presences and promotes via the stream-mode threshold.
    StreamedBatch batch2;
    batch2.firstLineNumber = 7;
    for (size_t i = 0; i < 4; ++i)
    {
        const char *value = (i % 2 == 0) ? "info" : "warn";
        batch2.lines.push_back(MakeLine(keys, *sourcePtr, {{"feature", LogValue{std::string(value)}}}));
    }
    table.AppendBatch(std::move(batch2));

    REQUIRE(table.RowCount() == 10);
    const auto &columns = table.Configuration().Configuration().columns;
    const auto featureCol = std::ranges::find_if(columns, [](const auto &c) { return c.header == "feature"; });
    REQUIRE(featureCol != columns.end());
    CHECK(featureCol->type == LogConfiguration::Type::Enumeration);
    const KeyId featureKey = keys.Find("feature");
    REQUIRE(featureKey != INVALID_KEY_ID);
    REQUIRE(table.EnumDictionaries().Contains(featureKey));
    const EnumDictionary *dict = table.EnumDictionaries().Find(featureKey);
    REQUIRE(dict != nullptr);
    CHECK(dict->Size() == 2);
}

TEST_CASE(
    "LogTable -- a loaded Type::Any column with autoDetect=false opts the user out of auto-detection",
    "[log_table][append_batch][enum][auto_detect_opt_out]"
)
{
    // To opt out of auto-detection per-column, save the column with
    // `autoDetect = false`. The candidate-scan gate
    // (`IsEnumPassEligible`) requires `Type::Any + autoDetect == true`.
    const TestLogFile testFile("enum_loaded_any_opt_out.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "level",
         .keys = {"level"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Any,
         .parseFormats = {},
         .visible = true,
         .levelMapping = {},
         .autoDetect = false}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    // Plenty of rows would normally promote the column; the
    // user-locked `Type::Any` keeps it as text.
    constexpr size_t ROWS = 320;
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "level", {"info", "warn", "error"}, 1, ROWS, false));

    REQUIRE(table.RowCount() == ROWS);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Any);
    CHECK_FALSE(table.Configuration().Configuration().columns[0].autoDetect);
    const KeyId levelKey = keys.Find("level");
    REQUIRE(levelKey != INVALID_KEY_ID);
    CHECK_FALSE(table.EnumDictionaries().Contains(levelKey));
}

TEST_CASE(
    "LogTable -- user-pinned enum columns survive across batches without auto-detection running",
    "[log_table][append_batch][enum][user_pinned]"
)
{
    // A user-pinned `Type::Enumeration` column still encodes as
    // `DictRef`. Non-level key (`category`) so the data doesn't flip
    // to `Type::Level` -- that path has its own tests.
    const TestLogFile testFile("enum_user_pinned.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "category",
         .keys = {"category"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Enumeration,
         .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "category", {"info", "warn"}, 1, 6, true));

    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);
    const KeyId categoryKey = keys.Find("category");
    REQUIRE(categoryKey != INVALID_KEY_ID);
    REQUIRE(table.EnumDictionaries().Contains(categoryKey));
    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        CHECK(table.Data().Lines()[row].IsDictRef(categoryKey));
    }
}

namespace
{

/// Build a batch where every row carries the same numeric kind under
/// `columnKey`. Used by the no-string-bail routing tests.
template <typename T>
StreamedBatch BuildNumericBatch(
    KeyIndex &keys, LineSource &source, const std::string &columnKey, T value, size_t rowCount, bool announceNewKey
)
{
    std::vector<std::vector<std::pair<std::string, LogValue>>> rows;
    rows.reserve(rowCount);
    for (size_t i = 0; i < rowCount; ++i)
    {
        rows.push_back({{columnKey, LogValue{value}}});
    }
    StreamedBatch batch = BuildStreamedBatch(keys, source, rows, announceNewKey ? 0 : keys.Size(), 1);
    if (!announceNewKey)
    {
        batch.newKeys.clear();
    }
    return batch;
}

} // namespace

TEST_CASE(
    "LogTable -- no-string bail routes int-only candidates to Type::Integer",
    "[log_table][append_batch][enum][routing][integer]"
)
{
    // The candidate scan counts `Int64`/`UInt64` tags so the no-string
    // bail flips int-only columns straight to `Type::Integer`.
    const TestLogFile testFile("enum_no_string_int.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();
    // 8 rows comfortably exceeds scanCap (4) so the bail fires.
    table.AppendBatch(BuildNumericBatch<int64_t>(keys, *sourcePtr, "count", 42, 8, true));

    REQUIRE(table.Configuration().Configuration().columns.size() == 1);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Integer);
}

TEST_CASE(
    "LogTable -- no-string bail routes double-only candidates to Type::Floating",
    "[log_table][append_batch][enum][routing][double]"
)
{
    const TestLogFile testFile("enum_no_string_double.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();
    table.AppendBatch(BuildNumericBatch<double>(keys, *sourcePtr, "ratio", 1.5, 8, true));

    REQUIRE(table.Configuration().Configuration().columns.size() == 1);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Floating);
}

TEST_CASE(
    "LogTable -- no-string bail routes mixed numeric candidates to Type::Number",
    "[log_table][append_batch][enum][routing][number]"
)
{
    // Mixed int + double observations route to `Type::Number`.
    const TestLogFile testFile("enum_no_string_mixed.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    StreamedBatch batch;
    batch.firstLineNumber = 1;
    batch.newKeys = {"value"};
    for (size_t i = 0; i < 8; ++i)
    {
        if (i % 2 == 0)
        {
            batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"value", LogValue{int64_t{17}}}}));
        }
        else
        {
            batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"value", LogValue{2.5}}}));
        }
    }
    table.AppendBatch(std::move(batch));

    REQUIRE(table.Configuration().Configuration().columns.size() == 1);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Number);
}

TEST_CASE(
    "LogTable -- bool-only column auto-detects to Type::Boolean", "[log_table][append_batch][enum][routing][boolean]"
)
{
    // Bool-only columns route through the dedicated `Type::Boolean`
    // branch instead of the historical `Type::Any` fallback.
    const TestLogFile testFile("enum_no_string_bool.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    StreamedBatch batch;
    batch.firstLineNumber = 1;
    batch.newKeys = {"flag"};
    for (size_t i = 0; i < 8; ++i)
    {
        batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"flag", LogValue{i % 2 == 0}}}));
    }
    table.AppendBatch(std::move(batch));

    REQUIRE(table.Configuration().Configuration().columns.size() == 1);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Boolean);
}

TEST_CASE(
    "LogTable -- bool mixed with numerics still routes to Type::Any", "[log_table][append_batch][enum][routing][any]"
)
{
    // Mixed bool + integer has no single specialised widget, so the
    // no-string bail falls through to `Type::Any`.
    const TestLogFile testFile("enum_no_string_bool_mixed.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    StreamedBatch batch;
    batch.firstLineNumber = 1;
    batch.newKeys = {"flag"};
    for (size_t i = 0; i < 8; ++i)
    {
        if (i % 2 == 0)
        {
            batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"flag", LogValue{i % 4 == 0}}}));
        }
        else
        {
            batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"flag", LogValue{static_cast<int64_t>(i)}}}));
        }
    }
    table.AppendBatch(std::move(batch));

    REQUIRE(table.Configuration().Configuration().columns.size() == 1);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Any);
}

namespace
{

/// Build a `LogConfigurationManager` pre-loaded with an auto-detect
/// candidate column (`Type::Any + autoDetect = true`) for `key`,
/// bypassing `Update`.
LogConfigurationManager MakeUnknownColumnManager(const std::string &key)
{
    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = key, .keys = {key}, .printFormat = "{}", .type = LogConfiguration::Type::Any, .parseFormats = {}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());
    return mgr;
}

} // namespace

TEST_CASE(
    "LogTable -- static-mode high-cardinality column routes to Type::String",
    "[log_table][update][enum][routing][cardinality_bail][static_mode]"
)
{
    // High-cardinality columns route to `Type::String` via the
    // dict-cap kill in `EnumCandidateTracker::Observe` (cap = 64).
    // The cardinality ratio bail rarely fires under the default cap;
    // the cap kill catches unique columns first.
    const TestLogFile testFile("enum_cardinality_bail.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    KeyIndex keys;
    std::vector<LogLine> lines;
    // 600 distinct rows exceed the 64-entry dict cap inside `Observe`,
    // kill the tracker, and flip the column to `Type::String`.
    lines.reserve(600);
    for (size_t i = 0; i < 600; ++i)
    {
        lines.push_back(MakeLine(keys, *sourcePtr, {{"id", std::string("id-" + std::to_string(i))}}));
    }
    LogData data(std::move(source), std::move(lines), std::move(keys));
    data.MarkTimestampsParsed();

    LogTable table(std::move(data), MakeUnknownColumnManager("id"));

    REQUIRE(table.RowCount() == 600);
    REQUIRE(table.ColumnCount() == 1);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::String);
}

TEST_CASE(
    "LogTable::FinalizeAutoDetection -- promotes small-file candidates the per-batch threshold missed",
    "[log_table][finalize][enum][small_file]"
)
{
    // Static parse with only 4 rows: per-batch promotion never fires (4096
    // row threshold). `FinalizeAutoDetection` applies the permissive
    // end-of-parse rule and promotes the column anyway.
    const TestLogFile testFile("enum_finalize_small.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    KeyIndex keys;
    std::vector<LogLine> lines;
    lines.push_back(MakeLine(keys, *sourcePtr, {{"tier", std::string("alpha")}}));
    lines.push_back(MakeLine(keys, *sourcePtr, {{"tier", std::string("beta")}}));
    lines.push_back(MakeLine(keys, *sourcePtr, {{"tier", std::string("alpha")}}));
    lines.push_back(MakeLine(keys, *sourcePtr, {{"tier", std::string("beta")}}));
    LogData data(std::move(source), std::move(lines), std::move(keys));
    data.MarkTimestampsParsed();

    LogTable table(std::move(data), MakeUnknownColumnManager("tier"));

    REQUIRE(table.RowCount() == 4);
    REQUIRE(table.ColumnCount() == 1);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);
    const KeyId tierKey = table.Keys().Find("tier");
    REQUIRE(tierKey != INVALID_KEY_ID);
    REQUIRE(table.EnumDictionaries().Contains(tierKey));
    for (size_t row = 0; row < table.RowCount(); ++row)
    {
        CHECK(table.Data().Lines()[row].IsDictRef(tierKey));
    }
}

TEST_CASE(
    "LogTable::FinalizeAutoDetection -- single-row candidate stays a candidate",
    "[log_table][finalize][enum][small_file]"
)
{
    // The permissive rule needs `rowsObserved >= 2`; a 1-row file
    // leaves the column at `Type::Any + autoDetect = true` so a later
    // re-load can decide.
    const TestLogFile testFile("enum_finalize_one_row.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    KeyIndex keys;
    std::vector<LogLine> lines;
    lines.push_back(MakeLine(keys, *sourcePtr, {{"tier", std::string("alpha")}}));
    LogData data(std::move(source), std::move(lines), std::move(keys));
    data.MarkTimestampsParsed();

    LogTable table(std::move(data), MakeUnknownColumnManager("tier"));

    REQUIRE(table.RowCount() == 1);
    REQUIRE(table.ColumnCount() == 1);
    const auto &col = table.Configuration().Configuration().columns[0];
    CHECK(col.type == LogConfiguration::Type::Any);
    CHECK(col.autoDetect);
}

TEST_CASE(
    "LogTable -- static-mode constructor finalize promotes mid-size files", "[log_table][static_mode][enum][threshold]"
)
{
    // 100 rows do not hit the static per-batch threshold (4096); the
    // constructor's `FinalizeAutoDetection` sweep promotes them anyway.
    // Also confirms the constructor leaves `mIsStreaming = false`.
    const TestLogFile testFile("enum_static_threshold.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    KeyIndex keys;
    std::vector<LogLine> lines;
    lines.reserve(100);
    for (size_t i = 0; i < 100; ++i)
    {
        lines.push_back(MakeLine(keys, *sourcePtr, {{"tier", std::string(i % 2 == 0 ? "alpha" : "beta")}}));
    }
    LogData data(std::move(source), std::move(lines), std::move(keys));
    data.MarkTimestampsParsed();

    LogTable table(std::move(data), MakeUnknownColumnManager("tier"));

    REQUIRE(table.RowCount() == 100);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);
}

// Regression: the explicit `LogTable` move ctor / assignment used
// to drop `mLastBatchDemotedKeys`, so a table moved between an
// `AppendBatch` that demoted a column and the `LogModel` consumer
// silently lost the "demoted-this-batch" trail. `LogModel` relies
// on the trail to emit `enumColumnsChanged(Demoted)` for the
// `Unknown -> Enumeration -> String` in-batch case where no
// dictionary survives on either side.
TEST_CASE(
    "LogTable -- move ctor and assignment carry mLastBatchDemotedKeys forward",
    "[log_table][move][last_batch_demoted_keys][regression]"
)
{
    constexpr uint16_t TINY_CAP = 2;

    auto driveInBatchPromoteThenDemote = [&](LogTable &table) {
        const TestLogFile fixture("log_table_move_demoted_keys.json");
        fixture.Write("");
        auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(fixture.GetFilePath()));
        FileLineSource *sourcePtr = source.get();

        // Streaming entry point is what triggers
        // `DemoteColumnFromEnum` to populate `LastBatchDemotedKeys`
        // from `RunEnumPassForAppendBatch`.
        table.BeginStreaming(std::move(source));
        table.SetEnumValueCap(TINY_CAP);

        // Stream-mode key "level" with the 2-row promotion threshold:
        // `info`/`info` promotes, then the encode pass trips the
        // 2-cap on `warn`/`error`/`fatal` and demotes to
        // `Type::String` -- all in one `AppendBatch`.
        KeyIndex &keys = table.Keys();
        StreamedBatch batch;
        batch.firstLineNumber = 1;
        batch.newKeys.emplace_back("level");
        for (const std::string_view value : {"info", "info", "warn", "error", "fatal"})
        {
            batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"level", std::string(value)}}));
        }
        table.AppendBatch(std::move(batch));
    };

    SECTION("Move ctor preserves the demoted-keys trail")
    {
        LogConfiguration cfg;
        cfg.columns.push_back(
            {.header = "level",
             .keys = {"level"},
             .printFormat = "{}",
             .type = LogConfiguration::Type::Any,
             .parseFormats = {}}
        );
        const TestLogConfiguration cfgFile;
        cfgFile.Write(cfg);
        LogConfigurationManager mgr;
        mgr.Load(cfgFile.GetFilePath());

        LogTable source({}, std::move(mgr));
        driveInBatchPromoteThenDemote(source);
        REQUIRE_FALSE(source.LastBatchDemotedKeys().empty());
        const KeyId demotedKey = source.LastBatchDemotedKeys().front();

        const LogTable moved(std::move(source));
        REQUIRE(moved.LastBatchDemotedKeys().size() == 1);
        CHECK(moved.LastBatchDemotedKeys().front() == demotedKey);
        // Moved-from is emptied: safe to read, but no trail.
        CHECK(source.LastBatchDemotedKeys().empty()); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    }

    SECTION("Move assignment preserves the demoted-keys trail")
    {
        LogConfiguration cfg;
        cfg.columns.push_back(
            {.header = "level",
             .keys = {"level"},
             .printFormat = "{}",
             .type = LogConfiguration::Type::Any,
             .parseFormats = {}}
        );
        const TestLogConfiguration cfgFile;
        cfgFile.Write(cfg);
        LogConfigurationManager mgr;
        mgr.Load(cfgFile.GetFilePath());

        LogTable source({}, std::move(mgr));
        driveInBatchPromoteThenDemote(source);
        REQUIRE_FALSE(source.LastBatchDemotedKeys().empty());
        const KeyId demotedKey = source.LastBatchDemotedKeys().front();

        LogTable target;
        target = std::move(source);
        REQUIRE(target.LastBatchDemotedKeys().size() == 1);
        CHECK(target.LastBatchDemotedKeys().front() == demotedKey);
        CHECK(source.LastBatchDemotedKeys().empty()); // NOLINT(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
    }
}

TEST_CASE(
    "LogTable -- auto-detected `level` column with canonical values promotes to Type::Level",
    "[log_table][append_batch][level]"
)
{
    // Key matches `IsLogLevelKey` + canonical dict entries trigger
    // the `Enumeration -> Level` second-step promotion.
    const TestLogFile testFile("level_auto_promote.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "level", {"info", "warn", "error"}, 1, 6, true));

    REQUIRE(table.RowCount() == 6);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Level);
    // `BuildEnumBatch` cycles values, so row N % 3 picks the value.
    CHECK(table.GetLevelForRow(0, 0) == LogLevel::Info);
    CHECK(table.GetLevelForRow(1, 0) == LogLevel::Warn);
    CHECK(table.GetLevelForRow(2, 0) == LogLevel::Error);
}

TEST_CASE(
    "LogTable -- non-level column name does not promote to Type::Level despite canonical values",
    "[log_table][append_batch][level]"
)
{
    // Same dictionary as the previous test but key is `tier` (not a
    // level key). Promotion stops at `Type::Enumeration`.
    const TestLogFile testFile("level_no_name_match.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "tier", {"info", "warn", "error"}, 1, 6, true));

    REQUIRE(table.RowCount() == 6);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);
    CHECK_FALSE(table.GetLevelForRow(0, 0).has_value());
}

TEST_CASE(
    "LogTable -- `level` column with mostly non-canonical values stays Type::Enumeration",
    "[log_table][append_batch][level]"
)
{
    // Key matches but most dict entries don't resolve to canonical
    // levels, so the tolerance fails and the column stays Enumeration.
    const TestLogFile testFile("level_threshold.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    // 5 distinct entries, only `info` is canonical: `4 * 4 = 16` > 1,
    // so promotion is blocked.
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "level", {"info", "qux", "wat", "frob", "baz"}, 1, 10, true));

    REQUIRE(table.RowCount() == 10);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);
}

TEST_CASE(
    "LogTable -- user-pinned Type::Level column populates the rank cache and preserves raw bytes",
    "[log_table][append_batch][level][user_pinned]"
)
{
    // A pinned `Type::Level` column with `levelMapping` overrides
    // keeps raw user strings in the dictionary; the rank cache holds
    // the canonical mapping separately.
    const TestLogFile testFile("level_user_pinned.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "lvl",
         .keys = {"lvl"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Level,
         .parseFormats = {},
         .levelMapping = {{"PANIC", "Fatal"}, {"NOTICE", "Info"}}}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "lvl", {"NOTICE", "PANIC", "info"}, 1, 6, true));

    REQUIRE(table.RowCount() == 6);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Level);
    // BuildEnumBatch cycles through values modulo size: row 0/3 = NOTICE, 1/4 = PANIC, 2/5 = info.
    CHECK(table.GetLevelForRow(0, 0) == LogLevel::Info);
    CHECK(table.GetLevelForRow(1, 0) == LogLevel::Fatal);
    CHECK(table.GetLevelForRow(2, 0) == LogLevel::Info);

    // Raw strings preserved in the formatted-display path.
    CHECK(table.GetFormattedValue(0, 0) == "NOTICE");
    CHECK(table.GetFormattedValue(1, 0) == "PANIC");
}

TEST_CASE(
    "LogTable -- Type::Level column with a stray non-canonical value reports nullopt for that row",
    "[log_table][append_batch][level]"
)
{
    // 4 canonical + 1 unrecognized entries: `1 * 4 <= 4` so the
    // tolerance holds and the column promotes to `Type::Level`. The
    // unrecognized entry stays in the dictionary (display fidelity);
    // `GetLevelForRow` returns nullopt for its rows.
    const TestLogFile testFile("level_unmapped_value.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    // 5 distinct values; only `"qux"` is non-canonical.
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "level", {"info", "warn", "error", "fatal", "qux"}, 1, 10, true)
    );

    REQUIRE(table.RowCount() == 10);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Level);
    // BuildEnumBatch cycles values mod 5: row 0 = info, 1 = warn, 2 = error, 3 = fatal, 4 = qux.
    CHECK(table.GetLevelForRow(0, 0) == LogLevel::Info);
    CHECK(table.GetLevelForRow(1, 0) == LogLevel::Warn);
    CHECK(table.GetLevelForRow(2, 0) == LogLevel::Error);
    CHECK(table.GetLevelForRow(3, 0) == LogLevel::Fatal);
    CHECK_FALSE(table.GetLevelForRow(4, 0).has_value());
    // Raw "qux" remains the visible value.
    CHECK(table.GetFormattedValue(4, 0) == "qux");
}

TEST_CASE(
    "LogTable -- short-form key `l` + single-letter values {i,w,e,f} promote to Type::Level",
    "[log_table][append_batch][level][short_form]"
)
{
    // End-to-end coverage for the short-form aliases:
    //   - `l` is a level key (name match).
    //   - `i`/`w`/`e`/`f` are canonical level aliases (dict content).
    // The full pipeline (`PromoteColumnToEnum -> MaybePromoteToLevel
    // -> RefreshLevelRankCache -> GetLevelForRow`) must round-trip
    // the single-letter input to the canonical severity ordinal.
    const TestLogFile testFile("level_short_form.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "l", {"i", "w", "e", "f"}, 1, 8, true));

    REQUIRE(table.RowCount() == 8);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Level);
    // BuildEnumBatch cycles values mod 4: row 0 = i, 1 = w, 2 = e, 3 = f.
    CHECK(table.GetLevelForRow(0, 0) == LogLevel::Info);
    CHECK(table.GetLevelForRow(1, 0) == LogLevel::Warn);
    CHECK(table.GetLevelForRow(2, 0) == LogLevel::Error);
    CHECK(table.GetLevelForRow(3, 0) == LogLevel::Fatal);
    // Raw single-letter bytes preserved verbatim.
    CHECK(table.GetFormattedValue(0, 0) == "i");
    CHECK(table.GetFormattedValue(3, 0) == "f");
}

TEST_CASE(
    "LogTable -- short-form key `l` with non-level values stays Type::Enumeration (safety net)",
    "[log_table][append_batch][level][short_form]"
)
{
    // Single-letter keys are accepted by `IsLogLevelKey` despite
    // false-positive risk; the dict-content check is the safety net.
    // `l` with non-canonical values must stop at Enumeration.
    const TestLogFile testFile("level_short_form_safety_net.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    // None of these resolve via `ParseLevelName` / built-in aliases.
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "l", {"red", "green", "blue"}, 1, 6, true));

    REQUIRE(table.RowCount() == 6);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);
    CHECK_FALSE(table.GetLevelForRow(0, 0).has_value());
}

TEST_CASE(
    "LogTable::ComputeColumnTypeHealth counts present and type-matching slots per column",
    "[log_table][diagnostics][column_health]"
)
{
    // User-pinned `Type::Integer` column over data that mixes ints
    // and strings: the diagnostic should report all rows present and
    // only the int rows as matching, so the UI can flag the mismatch.
    const TestLogFile testFile("column_health_int_vs_string.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "value",
         .keys = {"value"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Integer,
         .parseFormats = {},
         .visible = true,
         .levelMapping = {},
         .autoDetect = false}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    // 5 rows total: 3 int slots, 1 string slot, 1 absent (monostate).
    StreamedBatch batch;
    batch.firstLineNumber = 1;
    batch.newKeys.emplace_back("value");
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"value", LogValue{static_cast<int64_t>(10)}}}));
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"value", LogValue{static_cast<int64_t>(20)}}}));
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"value", LogValue{static_cast<int64_t>(30)}}}));
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"value", LogValue{std::string("oops")}}}));
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"other", LogValue{static_cast<int64_t>(99)}}}));
    table.AppendBatch(std::move(batch));

    REQUIRE(table.RowCount() == 5);

    const auto health = table.ComputeColumnTypeHealth(0);
    CHECK(health.totalSlots == 5);
    CHECK(health.presentSlots == 4);  // 3 ints + 1 string; the absent row doesn't count
    CHECK(health.matchingSlots == 3); // only the 3 ints match `Type::Integer`

    // Out-of-range index yields a zero-initialised snapshot.
    const auto empty = table.ComputeColumnTypeHealth(99);
    CHECK(empty.totalSlots == 0);
    CHECK(empty.presentSlots == 0);
    CHECK(empty.matchingSlots == 0);
}

TEST_CASE(
    "LogTable::ComputeColumnTypeHealth treats Type::Any as matching every present slot",
    "[log_table][diagnostics][column_health][type_any]"
)
{
    const TestLogFile testFile("column_health_any.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogConfiguration cfg;
    cfg.columns.push_back(
        {.header = "value",
         .keys = {"value"},
         .printFormat = "{}",
         .type = LogConfiguration::Type::Any,
         .parseFormats = {},
         .visible = true,
         .levelMapping = {},
         .autoDetect = false}
    );
    const TestLogConfiguration cfgFile;
    cfgFile.Write(cfg);
    LogConfigurationManager mgr;
    mgr.Load(cfgFile.GetFilePath());

    LogTable table({}, std::move(mgr));
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();
    StreamedBatch batch;
    batch.firstLineNumber = 1;
    batch.newKeys.emplace_back("value");
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"value", LogValue{std::string("x")}}}));
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"value", LogValue{static_cast<int64_t>(7)}}}));
    table.AppendBatch(std::move(batch));

    const auto health = table.ComputeColumnTypeHealth(0);
    CHECK(health.totalSlots == 2);
    CHECK(health.presentSlots == 2);
    CHECK(health.matchingSlots == 2);
}

TEST_CASE(
    "LogTable -- renaming a column header mid-stream does not orphan the enum tracker",
    "[log_table][append_batch][enum][rename_header][regression]"
)
{
    // Regression: trackers and column health used to be keyed by
    // `column.header`, so a `SetColumnHeader` mid-session reset the
    // promotion budget and the column never promoted -- silently.
    // The keying now rides the canonical `KeyId`, which is invariant
    // under display-only renames.
    //
    // `STREAM_PROMOTION_MIN_ROWS == 2`, so promotion happens once
    // the tracker has seen two present slots. We sandwich the
    // rename between two single-row batches so neither side alone
    // would promote -- before the fix that meant the column
    // permanently stayed at `Type::Any` because batch-2 started
    // with a fresh, empty tracker keyed on the new header.
    const TestLogFile testFile("enum_rename_header.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));

    KeyIndex &keys = table.Keys();

    // Batch 1: a single row -- enough to register the tracker but
    // one short of the streaming promote threshold. The key is
    // chosen so `IsLogLevelKey` does NOT match, otherwise the
    // promote path routes to `Type::Level` and our assertion below
    // has to special-case it.
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "category", {"first"}, /*firstLineNumber*/ 1, 1, true));
    REQUIRE(table.Configuration().Configuration().columns.size() == 1);
    REQUIRE(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Any);
    REQUIRE(table.Configuration().Configuration().columns[0].autoDetect == true);

    // Rename the display header out from under the running tracker.
    table.Configuration().SetColumnHeader(0, "Display Name");

    // Batch 2: a second present row pushes `presenceCount` to 2 and
    // promotion fires *only* if the tracker survived the rename.
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "category", {"second"}, 2, 1, false));

    const auto &column = table.Configuration().Configuration().columns[0];
    CHECK(column.type == LogConfiguration::Type::Enumeration);
    CHECK(column.header == "Display Name"); // rename survives the promote
    const KeyId categoryKey = keys.Find("category");
    REQUIRE(categoryKey != INVALID_KEY_ID);
    CHECK(table.EnumDictionaries().Contains(categoryKey));
}

TEST_CASE(
    "LogTable::OnUserChangedColumnType -- leaving Type::Time clears the strftime format strings",
    "[log_table][on_user_changed_column_type][time][regression]"
)
{
    // Regression: a user who pins a column to `Type::Time` and then
    // changes their mind (Time -> Integer / Number / String / ...)
    // used to leave `Column::printFormat = "%F %H:%M:%S"` behind.
    // The new type's formatter is `fmt::vformat`, which treats `%F`
    // as a literal -- every freshly-arriving row then renders the
    // raw format string instead of its value. Clearing the format
    // pair on the way out keeps rendering sane.
    const TestLogFile testFile("on_user_changed_time_format_reset.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    // Bootstrap a single-column session and pin it to Time so the
    // back-fill seeds the default strftime formats.
    StreamedBatch batch;
    batch.firstLineNumber = 1;
    batch.newKeys.emplace_back("value");
    batch.lines.push_back(MakeLine(keys, *sourcePtr, {{"value", LogValue{std::string("not a timestamp")}}}));
    table.AppendBatch(std::move(batch));

    REQUIRE(table.Configuration().Configuration().columns.size() == 1);
    const auto originalType = table.Configuration().Configuration().columns[0].type;

    table.Configuration().SetColumnTypePair(0, LogConfiguration::Type::Time, false);
    table.OnUserChangedColumnType(0, originalType);
    {
        const auto &col = table.Configuration().Configuration().columns[0];
        CHECK(col.type == LogConfiguration::Type::Time);
        CHECK_FALSE(col.printFormat.empty()); // seeded default
        CHECK_FALSE(col.parseFormats.empty()); // seeded default
    }

    // Time -> Integer: the format strings must NOT survive into the
    // new type or `fmt::vformat` will print the literal format on
    // every row.
    table.Configuration().SetColumnTypePair(0, LogConfiguration::Type::Integer, false);
    table.OnUserChangedColumnType(0, LogConfiguration::Type::Time);
    {
        const auto &col = table.Configuration().Configuration().columns[0];
        CHECK(col.type == LogConfiguration::Type::Integer);
        CHECK(col.printFormat == "{}");
        CHECK(col.parseFormats.empty());
    }
}

TEST_CASE(
    "LogTable::DemoteColumnFromEnum -- recordForBatch=false keeps the user-edit demote out of the batch vector",
    "[log_table][demote][on_user_changed_column_type][regression]"
)
{
    // Regression: `LastBatchDemotedKeys()` is documented as
    // "demotions during the *most recent batch*". `OnUserChangedColumnType`
    // demotes outside a batch when the user picks a non-enum type
    // from the editor; the GUI emits its own `enumColumnsChanged`
    // signal from `LogModel::ApplyColumnTypeEdit`, so leaving the
    // canonical KeyId in `mLastBatchDemotedKeys` would risk a
    // double-emit if a consumer queried it before the next batch
    // cleared the vector. The fix passes `recordForBatch=false`
    // from `OnUserChangedColumnType`.
    const TestLogFile testFile("demote_record_for_batch.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    FileLineSource *sourcePtr = source.get();

    LogTable table;
    table.BeginStreaming(std::move(source));
    KeyIndex &keys = table.Keys();

    // Promote `category` to Enumeration via the streaming detector.
    table.AppendBatch(BuildEnumBatch(keys, *sourcePtr, "category", {"a", "b"}, 1, 2, true));
    REQUIRE(table.Configuration().Configuration().columns.size() == 1);
    REQUIRE(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::Enumeration);

    // The streaming detector left the demote vector clean.
    CHECK(table.LastBatchDemotedKeys().empty());

    // User picks Type::String -- `OnUserChangedColumnType` runs the
    // demote walk but must NOT touch `mLastBatchDemotedKeys`.
    table.Configuration().SetColumnTypePair(0, LogConfiguration::Type::String, false);
    table.OnUserChangedColumnType(0, LogConfiguration::Type::Enumeration);
    CHECK(table.Configuration().Configuration().columns[0].type == LogConfiguration::Type::String);
    CHECK(table.LastBatchDemotedKeys().empty());
}
