#include "common.hpp"

#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_table.hpp>
#include <loglib/log_value.hpp>
#include <loglib/stream_log_line.hpp>
#include <loglib/streaming_log_sink.hpp>

#include <catch2/catch_all.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace loglib;

TEST_CASE("StreamLineReference exposes path / line number / raw line", "[stream_log_line]")
{
    StreamLineReference ref("/var/log/app.log", "raw bytes here", 42);

    CHECK(ref.GetPath().string() == "/var/log/app.log");
    CHECK(ref.GetLineNumber() == 42);
    CHECK(ref.GetLine() == "raw bytes here");
}

TEST_CASE("StreamLineReference::ShiftLineNumber bumps the stored id", "[stream_log_line]")
{
    StreamLineReference ref("/tmp/x", "x", 1);
    ref.ShiftLineNumber(99);
    CHECK(ref.GetLineNumber() == 100);
}

TEST_CASE("StreamLogLine round-trips every LogValue alternative", "[stream_log_line]")
{
    KeyIndex keys;
    const KeyId monoKey = keys.GetOrInsert("none");
    const KeyId stringKey = keys.GetOrInsert("string");
    const KeyId intKey = keys.GetOrInsert("int");
    const KeyId uintKey = keys.GetOrInsert("uint");
    const KeyId doubleKey = keys.GetOrInsert("double");
    const KeyId boolKey = keys.GetOrInsert("bool");
    const KeyId timeKey = keys.GetOrInsert("time");

    const TimeStamp expectedTime{std::chrono::microseconds{123'456'789}};

    std::vector<std::pair<KeyId, LogValue>> sortedValues;
    sortedValues.emplace_back(monoKey, LogValue{std::monostate{}});
    sortedValues.emplace_back(stringKey, LogValue{std::string("hello")});
    sortedValues.emplace_back(intKey, LogValue{int64_t{-7}});
    sortedValues.emplace_back(uintKey, LogValue{uint64_t{42}});
    sortedValues.emplace_back(doubleKey, LogValue{3.14});
    sortedValues.emplace_back(boolKey, LogValue{true});
    sortedValues.emplace_back(timeKey, LogValue{expectedTime});
    std::sort(sortedValues.begin(), sortedValues.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

    StreamLogLine line(std::move(sortedValues), keys, StreamLineReference{"src", "raw", 1});

    CHECK(std::holds_alternative<std::monostate>(line.GetValue(monoKey)));
    CHECK(std::get<std::string>(line.GetValue(stringKey)) == "hello");
    CHECK(std::get<int64_t>(line.GetValue(intKey)) == -7);
    CHECK(std::get<uint64_t>(line.GetValue(uintKey)) == 42U);
    CHECK(std::get<double>(line.GetValue(doubleKey)) == Catch::Approx(3.14));
    CHECK(std::get<bool>(line.GetValue(boolKey)) == true);
    CHECK(std::get<TimeStamp>(line.GetValue(timeKey)) == expectedTime);

    // Lookup by key name routes through the bound `KeyIndex`.
    CHECK(std::get<std::string>(line.GetValue(std::string("string"))) == "hello");

    // Unknown key returns monostate without throwing.
    CHECK(std::holds_alternative<std::monostate>(line.GetValue(std::string("unknown_key"))));
    CHECK(std::holds_alternative<std::monostate>(line.GetValue(KeyId{99})));
}

TEST_CASE("StreamLogLine promotes string_view payloads to owned strings", "[stream_log_line]")
{
    KeyIndex keys;
    const KeyId k = keys.GetOrInsert("k");

    std::string scratch = "transient";
    std::vector<std::pair<KeyId, LogValue>> values;
    values.emplace_back(k, LogValue{std::string_view(scratch)});

    StreamLogLine line(std::move(values), keys, StreamLineReference{"src", "raw", 1});

    // After construction the line must own its string bytes; the original
    // backing buffer is allowed to disappear.
    scratch.clear();
    scratch.shrink_to_fit();

    const LogValue value = line.GetValue(k);
    CHECK(std::holds_alternative<std::string>(value));
    CHECK(std::get<std::string>(value) == "transient");
}

TEST_CASE("StreamLogLine SetValue inserts and replaces fields in sorted order", "[stream_log_line]")
{
    KeyIndex keys;
    const KeyId a = keys.GetOrInsert("a");
    const KeyId b = keys.GetOrInsert("b");
    const KeyId c = keys.GetOrInsert("c");

    std::vector<std::pair<KeyId, LogValue>> values;
    values.emplace_back(a, LogValue{int64_t{1}});
    values.emplace_back(c, LogValue{int64_t{3}});

    StreamLogLine line(std::move(values), keys, StreamLineReference{"src", "raw", 1});

    line.SetValue(b, LogValue{int64_t{2}});
    CHECK(std::get<int64_t>(line.GetValue(a)) == 1);
    CHECK(std::get<int64_t>(line.GetValue(b)) == 2);
    CHECK(std::get<int64_t>(line.GetValue(c)) == 3);

    // Replace existing.
    line.SetValue(b, LogValue{int64_t{20}});
    CHECK(std::get<int64_t>(line.GetValue(b)) == 20);

    // SetValue by key name uses the bound KeyIndex.
    line.SetValue(std::string("a"), LogValue{int64_t{10}});
    CHECK(std::get<int64_t>(line.GetValue(a)) == 10);

    // Unknown key throws.
    CHECK_THROWS(line.SetValue(std::string("nope"), LogValue{int64_t{0}}));
}

TEST_CASE("StreamLogLine map ctor sorts pairs by KeyId", "[stream_log_line]")
{
    KeyIndex keys;
    LogMap input;
    input.emplace("zeta", LogValue{int64_t{26}});
    input.emplace("alpha", LogValue{std::string("first")});
    input.emplace("middle", LogValue{double{1.5}});

    StreamLogLine line(input, keys, StreamLineReference{"src", "raw", 1});

    const auto &indexed = line.IndexedValues();
    REQUIRE(indexed.size() == 3);
    for (size_t i = 1; i < indexed.size(); ++i)
    {
        CHECK(indexed[i - 1].first < indexed[i].first);
    }
    CHECK(line.GetKeys().size() == 3);
    CHECK(std::get<std::string>(line.GetValue(std::string("alpha"))) == "first");
    CHECK(std::get<double>(line.GetValue(std::string("middle"))) == Catch::Approx(1.5));
    CHECK(std::get<int64_t>(line.GetValue(std::string("zeta"))) == 26);
}

TEST_CASE("LogTable surfaces both LogLine and StreamLogLine rows from a mixed batch", "[stream_log_line][log_table]")
{
    // Build a static-side LogLine row (file-backed) and a stream-side
    // StreamLogLine row in the same `KeyIndex`, append both via a single
    // `StreamedBatch`, and verify `LogTable::GetFormattedValue` produces
    // the same display string for the same logical value on both rows
    // (PRD task 2.7 last paragraph).
    TestLogFile testFile;
    testFile.Write("file-line-payload\n");
    auto logFile = testFile.CreateLogFile();

    LogConfiguration config;
    config.columns.push_back({"Message", {"msg"}, "{}", LogConfiguration::Type::any, {}});
    TestLogConfiguration testConfig;
    testConfig.Write(config);
    LogConfigurationManager manager;
    manager.Load(testConfig.GetFilePath());

    LogTable table(LogData{}, std::move(manager));
    table.BeginStreaming(std::move(logFile));

    KeyIndex &tableKeys = table.Keys();
    const KeyId msgKey = tableKeys.GetOrInsert("msg");

    LogFile &fileRef = *table.Data().Files().front();

    // File-mode row.
    LogLine fileLine(
        std::vector<std::pair<KeyId, LogValue>>{{msgKey, LogValue{std::string("file-message")}}},
        tableKeys,
        LogFileReference(fileRef, 0)
    );

    // Stream-mode row.
    StreamLogLine streamLine(
        std::vector<std::pair<KeyId, LogValue>>{{msgKey, LogValue{std::string("stream-message")}}},
        tableKeys,
        StreamLineReference{"<live>", "stream-raw", 1}
    );

    StreamedBatch batch;
    batch.lines.push_back(std::move(fileLine));
    batch.streamLines.push_back(std::move(streamLine));
    batch.newKeys = {"msg"}; // mirrors how `RunParserPipeline::emitNewKeysInto` populates this
    batch.firstLineNumber = 1;

    table.AppendBatch(std::move(batch));

    REQUIRE(table.RowCount() == 2);
    REQUIRE(table.ColumnCount() >= 1);
    CHECK(table.Data().Lines().size() == 1);
    CHECK(table.Data().StreamLines().size() == 1);

    CHECK(table.GetFormattedValue(0, 0) == "file-message");
    CHECK(table.GetFormattedValue(1, 0) == "stream-message");
}

TEST_CASE("LogData::AppendBatch with StreamLogLine rows does not require a file", "[stream_log_line][log_data]")
{
    LogData data;
    KeyIndex &keys = data.Keys();
    const KeyId k = keys.GetOrInsert("level");

    std::vector<StreamLogLine> stream;
    stream.emplace_back(
        std::vector<std::pair<KeyId, LogValue>>{{k, LogValue{std::string("info")}}},
        keys,
        StreamLineReference{"src", "raw", 1}
    );
    stream.emplace_back(
        std::vector<std::pair<KeyId, LogValue>>{{k, LogValue{std::string("error")}}},
        keys,
        StreamLineReference{"src", "raw", 2}
    );

    // No `mFiles` populated — this would historically trip
    // `assert(mFiles.size() == 1)` if the path went through the line-offsets
    // overload, but the `StreamLogLine` overload bypasses that branch
    // entirely (PRD 4.9.7 last paragraph, task 2.3).
    REQUIRE(data.Files().empty());
    data.AppendBatch(std::move(stream));

    REQUIRE(data.StreamLines().size() == 2);
    CHECK(std::get<std::string>(data.StreamLines()[0].GetValue(k)) == "info");
    CHECK(std::get<std::string>(data.StreamLines()[1].GetValue(k)) == "error");
    // The file-backed row vector is still empty.
    CHECK(data.Lines().empty());
    // No phantom files have been added.
    CHECK(data.Files().empty());
}

TEST_CASE("LogData rebinds StreamLogLine KeyIndex back-pointers on move", "[stream_log_line][log_data]")
{
    LogData data;
    {
        KeyIndex &keys = data.Keys();
        const KeyId k = keys.GetOrInsert("k");

        std::vector<StreamLogLine> stream;
        stream.emplace_back(
            std::vector<std::pair<KeyId, LogValue>>{{k, LogValue{std::string("v")}}},
            keys,
            StreamLineReference{"src", "raw", 1}
        );
        data.AppendBatch(std::move(stream));
    }

    LogData moved = std::move(data);
    REQUIRE(moved.StreamLines().size() == 1);
    // After the move, the line's `KeyIndex` back-pointer must reference
    // the moved-into `LogData`'s `mKeys`. Looking up by name is the
    // observable check (it routes through the bound KeyIndex).
    const LogValue value = moved.StreamLines().front().GetValue(std::string("k"));
    CHECK(std::get<std::string>(value) == "v");
}
