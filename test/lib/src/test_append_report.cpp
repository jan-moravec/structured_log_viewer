#include "common.hpp"
#include "table_fixture.hpp"

#include <loglib/core/file_line_source.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_level.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/schema/log_configuration.hpp>
#include <loglib/table/log_table.hpp>

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace loglib;

namespace
{

using Fields = std::vector<std::pair<std::string, LogValue>>;

LogLine MakeLine(KeyIndex &keys, LineSource &source, const Fields &fields)
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

// A batch of the given rows. Keys that the index did not hold before the call become newKeys, in id order.
RowBatch MakeBatch(KeyIndex &keys, LineSource &source, const std::vector<Fields> &rows, size_t firstLineNumber)
{
    const size_t keyCountBefore = keys.Size();
    RowBatch batch;
    batch.firstLineNumber = firstLineNumber;
    for (const Fields &fields : rows)
    {
        batch.lines.push_back(MakeLine(keys, source, fields));
    }
    for (size_t id = keyCountBefore; id < keys.Size(); ++id)
    {
        batch.newKeys.emplace_back(keys.KeyOf(static_cast<KeyId>(id)));
    }
    return batch;
}

TimeStamp TimeAt(int seconds)
{
    return std::chrono::time_point_cast<std::chrono::microseconds>(
        std::chrono::sys_days{std::chrono::year{2024} / std::chrono::January / 15} + std::chrono::seconds{seconds}
    );
}

LogConfiguration LevelConfiguration(std::vector<std::string> keys)
{
    LogConfiguration configuration;
    configuration.columns.push_back(
        Column{
            .header = keys.front(),
            .keys = std::move(keys),
            .printFormat = "{}",
            .type = ColumnType::Level,
            .parseFormats = {},
        }
    );
    return configuration;
}

// ---------------------------------------------------------------------------------------------------------------------
// A table fed batch by batch. Each call returns the report as text, so a test can write out what it expects:
// "Promoted 2 backfill" for an event and "move 3->1" for a placement that was applied.
// ---------------------------------------------------------------------------------------------------------------------

using BatchMaker = std::function<RowBatch(KeyIndex &, LineSource &)>;
using Described = std::vector<std::string>;

const char *ChangeName(EnumColumnChange change)
{
    switch (change)
    {
    case EnumColumnChange::Grew:
        return "Grew";
    case EnumColumnChange::Promoted:
        return "Promoted";
    case EnumColumnChange::Demoted:
        return "Demoted";
    }
    return "?";
}

class Stream
{
public:
    explicit Stream(const std::string &name, bool streaming = true, uint16_t enumCap = 0)
        : mFile(name + ".json")
    {
        InitializeTimezoneData();
        mFile.Write("");
        auto source = std::make_unique<FileLineSource>(mFile.GetFilePath());
        mSource = source.get();
        if (enumCap != 0)
        {
            mTable.SetEnumValueCap(enumCap);
        }
        if (streaming)
        {
            mTable.BeginStreaming(std::move(source));
        }
        else
        {
            mTable.AppendStreaming(std::move(source));
        }
    }

    Described Append(const BatchMaker &make)
    {
        const LogTable::AppendBatchResult result = mTable.AppendBatch(make(mTable.Keys(), *mSource));
        CHECK(result.errors.empty());
        return Take(result.enumEvents, result.placements);
    }

    Described Finalize()
    {
        const LogTable::FinalizeResult result = mTable.FinalizeAutoDetection();
        return Take(result.enumEvents, result.placements);
    }

    // Every event reported so far, for the checks that need a level mapping.
    [[nodiscard]] const std::vector<EnumColumnEvent> &Events() const noexcept
    {
        return mAllEvents;
    }

    [[nodiscard]] LogTable &Table() noexcept
    {
        return mTable;
    }

    [[nodiscard]] FileLineSource *Source() noexcept
    {
        return mSource;
    }

private:
    Described Take(const std::vector<EnumColumnEvent> &events, const std::vector<ColumnPlacement> &placements)
    {
        Described described;
        for (const EnumColumnEvent &event : events)
        {
            described.push_back(
                std::string(ChangeName(event.change)) + " " + std::to_string(event.column) +
                (event.causedByBackfill ? " backfill" : "")
            );
            mAllEvents.push_back(event);
        }
        std::vector<ColumnPlacement> pending(placements);
        for (size_t i = 0; i < pending.size(); ++i)
        {
            if (const auto move = mTable.ResolvePlacement(pending[i]); move.has_value())
            {
                described.push_back("move " + std::to_string(move->from) + "->" + std::to_string(move->to));
                mTable.MoveColumn(move->from, move->to);
                for (size_t next = i + 1; next < pending.size(); ++next)
                {
                    pending[next].column = move->Follow(pending[next].column);
                }
            }
        }
        return described;
    }

    TestLogFile mFile;
    FileLineSource *mSource = nullptr;
    LogTable mTable;
    std::vector<EnumColumnEvent> mAllEvents;
};

} // namespace

TEST_CASE("Level rank cache is found when the first configured key appears after an alias", "[append_report][c20]")
{
    const TestLogFile testFile("append_report_c20.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(testFile.GetFilePath());
    FileLineSource *sourcePtr = source.get();

    LogTable table(LevelConfiguration({"level", "severity"}));
    table.AppendStreaming(std::move(source));

    // Only the alias "severity" appears; "level" is never interned.
    const std::vector<Fields> rows = {
        {{"severity", std::string("info")}},
        {{"severity", std::string("warn")}},
        {{"severity", std::string("error")}},
    };
    AppendFixtureBatch(table, MakeBatch(table.Keys(), *sourcePtr, rows, 1));

    REQUIRE(table.RowCount() == 3);
    REQUIRE(table.Configuration().Configuration().columns[0].type == ColumnType::Level);
    CHECK(table.GetLevelForRow(0, 0) == LogLevel::Info);
    CHECK(table.GetLevelForRow(1, 0) == LogLevel::Warn);
    CHECK(table.GetLevelForRow(2, 0) == LogLevel::Error);
    const std::vector<LogLevel> *ranks = table.LevelRankCache(0);
    REQUIRE(ranks != nullptr);
    CHECK(ranks->size() == 3);
}

TEST_CASE("The append report of a static-like load in two batch sizes", "[append_report][events]")
{
    const size_t batchSize = GENERATE(size_t{1000}, size_t{4096});
    constexpr size_t TOTAL_ROWS = 4200;
    const std::vector<std::string> levels = {"info", "warn", "error", "debug"};

    Stream stream("append_report_static_" + std::to_string(batchSize));
    std::vector<Described> steps;
    for (size_t first = 0; first < TOTAL_ROWS; first += batchSize)
    {
        const size_t last = std::min(first + batchSize, TOTAL_ROWS);
        steps.push_back(stream.Append([&](KeyIndex &keys, LineSource &source) {
            std::vector<Fields> rows;
            for (size_t i = first; i < last; ++i)
            {
                rows.push_back({
                    {"msg", std::string("message ") + std::to_string(i)},
                    {"level", levels[i % levels.size()]},
                    {"timestamp", TimeAt(static_cast<int>(i))},
                    {"host", std::string("host") + std::to_string(i % 3)},
                });
            }
            return MakeBatch(keys, source, rows, first + 1);
        }));
    }
    steps.push_back(stream.Finalize());

    // The first batch decides everything: the message column stays text, the level and host columns become
    // enumerations, and the timestamp and then the level move to their places.
    const Described first{
        "Demoted 0",
        "Promoted 1 backfill",
        "Promoted 3 backfill",
        "move 2->0",
        "move 2->1",
    };
    std::vector<Described> expected{first};
    expected.resize(batchSize == 1000 ? 6 : 3);
    CHECK(steps == expected);

    const auto &columns = stream.Table().Configuration().Configuration().columns;
    REQUIRE(columns.size() == 4);
    CHECK(columns[0].header == "timestamp");
    CHECK(columns[1].header == "level");
    CHECK(columns[1].type == ColumnType::Level);
}

TEST_CASE("The append report of a column that grows and then demotes", "[append_report][events]")
{
    Stream stream("append_report_grow_demote", true, 4);
    const auto kinds = [](std::vector<std::string> values, size_t first) {
        return [values = std::move(values), first](KeyIndex &keys, LineSource &source) {
            std::vector<Fields> rows;
            rows.reserve(values.size());
            for (const std::string &value : values)
            {
                rows.push_back({{"kind", value}});
            }
            return MakeBatch(keys, source, rows, first);
        };
    };
    std::vector<Described> steps;
    steps.push_back(stream.Append(kinds({"a", "b", "a"}, 1)));
    steps.push_back(stream.Append(kinds({"c"}, 4)));
    steps.push_back(stream.Append(kinds({"d"}, 5)));
    steps.push_back(stream.Append(kinds({"e", "f"}, 6)));
    steps.push_back(stream.Finalize());

    const std::vector<Described> expected{
        {"Promoted 0 backfill"},
        {"Grew 0"},
        {"Grew 0"},
        {"Demoted 0"},
        {},
    };
    CHECK(steps == expected);
    CHECK(stream.Table().Configuration().Configuration().columns[0].type == ColumnType::String);
}

TEST_CASE("The append report of a column that is promoted and demoted in one batch", "[append_report][events]")
{
    Stream stream("append_report_same_batch", true, 2);
    const Described batch = stream.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(
            keys, source, {{{"kind", std::string("a")}}, {{"kind", std::string("b")}}, {{"kind", std::string("c")}}}, 1
        );
    });
    const Described finalize = stream.Finalize();

    CHECK(batch == Described{"Demoted 0"});
    CHECK(finalize.empty());
    CHECK(stream.Events().front().levelValuesBeforeDemotion.empty());
    CHECK(stream.Table().Configuration().Configuration().columns[0].type == ColumnType::String);
}

TEST_CASE("The append report carries the level values of a demoted Level column", "[append_report][events]")
{
    Stream stream("append_report_level_demote", true, 4);
    std::vector<Described> steps;
    steps.push_back(stream.Append([](KeyIndex &keys, LineSource &source) {
        std::vector<Fields> rows;
        for (const char *level : {"info", "warn", "error", "info", "warn", "error"})
        {
            rows.push_back({{"msg", std::string("m")}, {"level", std::string(level)}});
        }
        return MakeBatch(keys, source, rows, 1);
    }));
    steps.push_back(stream.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(keys, source, {{{"msg", std::string("m")}, {"level", std::string("x1")}}}, 7);
    }));
    steps.push_back(stream.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(
            keys,
            source,
            {
                {{"msg", std::string("m")}, {"level", std::string("x2")}},
                {{"msg", std::string("m")}, {"level", std::string("x3")}},
            },
            8
        );
    }));
    steps.push_back(stream.Finalize());

    const std::vector<Described> expected{
        {"Promoted 0 backfill", "Promoted 1 backfill"},
        {"Grew 1"},
        {"Demoted 1"},
        {},
    };
    CHECK(steps == expected);

    const auto demoted = std::ranges::find_if(stream.Events(), [](const EnumColumnEvent &event) {
        return event.change == EnumColumnChange::Demoted;
    });
    REQUIRE(demoted != stream.Events().end());
    REQUIRE(demoted->levelValuesBeforeDemotion.size() == 3);
    CHECK(demoted->levelValuesBeforeDemotion.at(LogLevel::Info) == std::vector<std::string>{"info"});
    CHECK(demoted->levelValuesBeforeDemotion.at(LogLevel::Warn) == std::vector<std::string>{"warn"});
    CHECK(demoted->levelValuesBeforeDemotion.at(LogLevel::Error) == std::vector<std::string>{"error"});
}

TEST_CASE(
    "The append report keeps the first of two Level-like columns in the canonical slot", "[append_report][events]"
)
{
    Stream stream("append_report_two_levels");
    std::vector<Described> steps;
    steps.push_back(stream.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(keys, source, {{{"body", std::string("hello")}}}, 1);
    }));
    steps.push_back(stream.Append([](KeyIndex &keys, LineSource &source) {
        std::vector<Fields> rows;
        for (const char *level : {"info", "warn", "error", "info", "warn", "error"})
        {
            rows.push_back({{"level", std::string(level)}, {"severity", std::string(level)}});
        }
        return MakeBatch(keys, source, rows, 2);
    }));
    steps.push_back(stream.Finalize());

    const std::vector<Described> expected{{}, {"Promoted 1 backfill", "Promoted 2 backfill"}, {}};
    CHECK(steps == expected);

    const auto &columns = stream.Table().Configuration().Configuration().columns;
    REQUIRE(columns.size() == 3);
    CHECK(columns[0].header == "body");
    CHECK(columns[1].header == "level");
    CHECK(columns[2].header == "severity");
    CHECK(columns[1].type == ColumnType::Level);
    CHECK(columns[2].type == ColumnType::Level);
}

TEST_CASE("The append report moves two new time columns of one batch to the front in order", "[append_report][events]")
{
    Stream stream("append_report_two_times");
    std::vector<Described> steps;
    steps.push_back(stream.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(keys, source, {{{"msg", std::string("first")}}}, 1);
    }));
    steps.push_back(stream.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(
            keys, source, {{{"msg", std::string("second")}, {"timestamp", TimeAt(1)}, {"time", TimeAt(2)}}}, 2
        );
    }));
    steps.push_back(stream.Finalize());

    // The message column holds two distinct values in two rows, so Finalize demotes it.
    const std::vector<Described> expected{{}, {"Promoted 0 backfill", "move 1->0", "move 2->0"}, {"Demoted 2"}};
    CHECK(steps == expected);

    const auto &columns = stream.Table().Configuration().Configuration().columns;
    REQUIRE(columns.size() == 3);
    CHECK(columns[0].type == ColumnType::Time);
    CHECK(columns[1].type == ColumnType::Time);
    CHECK(columns[2].header == "msg");
}

TEST_CASE("The append report of promotions and demotions at the end of a stream", "[append_report][events]")
{
    SECTION("an undecided column is promoted when the stream ends")
    {
        Stream stream("append_report_finalize_promote", false);
        const Described batch = stream.Append([](KeyIndex &keys, LineSource &source) {
            std::vector<Fields> rows;
            for (const char *level : {"info", "warn", "info", "warn"})
            {
                rows.push_back(
                    {{"msg", std::string("m")}, {"level", std::string(level)}, {"tier", std::string(level)}}
                );
            }
            return MakeBatch(keys, source, rows, 1);
        });
        const Described finalize = stream.Finalize();
        CHECK(batch.empty());
        CHECK(finalize == Described{"Promoted 0", "Promoted 1", "Promoted 2"});
        CHECK(stream.Table().Configuration().Configuration().columns[1].type == ColumnType::Level);
    }

    SECTION("a Level column that fails its health check is demoted with its level values")
    {
        Stream stream("append_report_finalize_demote", true);
        std::vector<Described> steps;
        steps.push_back(stream.Append([](KeyIndex &keys, LineSource &source) {
            std::vector<Fields> rows;
            for (const char *level : {"info", "warn", "error", "info", "warn", "error"})
            {
                rows.push_back({{"msg", std::string("m")}, {"level", std::string(level)}});
            }
            return MakeBatch(keys, source, rows, 1);
        }));
        steps.push_back(stream.Append([](KeyIndex &keys, LineSource &source) {
            std::vector<Fields> rows;
            rows.reserve(10);
            for (int i = 0; i < 10; ++i)
            {
                rows.push_back({{"msg", std::string("m")}, {"level", std::string(80, 'q')}});
            }
            return MakeBatch(keys, source, rows, 7);
        }));
        steps.push_back(stream.Finalize());

        const std::vector<Described> expected{{"Promoted 0 backfill", "Promoted 1 backfill"}, {}, {"Demoted 1"}};
        CHECK(steps == expected);

        const auto demoted = std::ranges::find_if(stream.Events(), [](const EnumColumnEvent &event) {
            return event.change == EnumColumnChange::Demoted;
        });
        REQUIRE(demoted != stream.Events().end());
        CHECK(demoted->levelValuesBeforeDemotion.size() == 3);
    }
}

TEST_CASE("ResolvePlacement gives nothing for a missing or already placed column", "[append_report][placement]")
{
    Stream stream("append_report_placement");
    static_cast<void>(stream.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(keys, source, {{{"msg", std::string("first")}}}, 1);
    }));
    static_cast<void>(stream.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(keys, source, {{{"msg", std::string("second")}, {"timestamp", TimeAt(1)}}}, 2);
    }));
    LogTable &table = stream.Table();
    REQUIRE(table.GetHeader(0) == "timestamp");

    // Already at the front, and a column that does not exist.
    CHECK_FALSE(table.ResolvePlacement({.kind = ColumnPlacement::Kind::TimeToFront, .column = 0}).has_value());
    CHECK_FALSE(table.ResolvePlacement({.kind = ColumnPlacement::Kind::TimeToFront, .column = 99}).has_value());
    CHECK_FALSE(
        table.ResolvePlacement({.kind = ColumnPlacement::Kind::LevelToCanonicalSlot, .column = 99}).has_value()
    );

    // A column that is not at the front gets a move to it.
    const auto toFront = table.ResolvePlacement({.kind = ColumnPlacement::Kind::TimeToFront, .column = 1});
    REQUIRE(toFront.has_value());
    CHECK(toFront->from == 1);
    CHECK(toFront->to == 0);

    // The canonical slot is taken by the column itself.
    CHECK_FALSE(table.ResolvePlacement({.kind = ColumnPlacement::Kind::LevelToCanonicalSlot, .column = 1}).has_value());
}

TEST_CASE("ColumnMove::Follow tracks where a move sends the other columns", "[append_report][placement]")
{
    const ColumnMove forward{.from = 3, .to = 0};
    CHECK(forward.Follow(3) == 0);
    CHECK(forward.Follow(0) == 1);
    CHECK(forward.Follow(2) == 3);
    CHECK(forward.Follow(4) == 4);

    const ColumnMove backward{.from = 0, .to = 2};
    CHECK(backward.Follow(0) == 2);
    CHECK(backward.Follow(1) == 0);
    CHECK(backward.Follow(2) == 1);
    CHECK(backward.Follow(3) == 3);
}

TEST_CASE("A new time column is placed without the table having streamed its keys", "[append_report][placement]")
{
    Stream stream("append_report_placement_no_streaming", /*streaming=*/false);
    static_cast<void>(stream.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(keys, source, {{{"msg", std::string("first")}}}, 1);
    }));
    LogTable &table = stream.Table();

    // Only the second batch declares the time key.
    const LogTable::AppendBatchResult result = table.AppendBatch(
        MakeBatch(table.Keys(), *stream.Source(), {{{"msg", std::string("second")}, {"timestamp", TimeAt(1)}}}, 2)
    );
    REQUIRE(result.placements.size() == 1);
    CHECK(result.placements.front().kind == ColumnPlacement::Kind::TimeToFront);
    CHECK(result.placements.front().column == 1);
    table.ApplyPlacements(result.placements);
    CHECK(table.GetHeader(0) == "timestamp");
}

TEST_CASE("AppendBatch returns the batch errors and the row and column counts", "[append_report][errors]")
{
    const TestLogFile testFile("append_report_errors.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(testFile.GetFilePath());
    FileLineSource *sourcePtr = source.get();
    LogTable table;
    table.BeginStreaming(std::move(source));

    RowBatch batch = MakeBatch(table.Keys(), *sourcePtr, {{{"a", std::string("x")}, {"b", std::string("y")}}}, 1);
    batch.errors.push_back(ParseError{.firstLine = 5, .lastLine = 6, .message = "bad record"});
    batch.errors.push_back(ParseError{.firstLine = 9, .lastLine = 9, .message = "worse record"});

    const LogTable::AppendBatchResult result = table.AppendBatch(std::move(batch));
    REQUIRE(result.errors.size() == 2);
    CHECK(result.errors[0] == ParseError{.firstLine = 5, .lastLine = 6, .message = "bad record"});
    CHECK(result.errors[1].message == "worse record");
    CHECK(result.rowsAdded == 1);
    CHECK(result.columnsAdded == 2);

    // A batch without errors reports none.
    const LogTable::AppendBatchResult second =
        table.AppendBatch(MakeBatch(table.Keys(), *sourcePtr, {{{"a", std::string("x")}}}, 2));
    CHECK(second.errors.empty());
    CHECK(second.rowsAdded == 1);
    CHECK(second.columnsAdded == 0);
}

TEST_CASE(
    "A pinned enumeration column keeps encoding known values after its dictionary is full", "[append_report][c21]"
)
{
    const TestLogFile testFile("append_report_c21.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(testFile.GetFilePath());
    FileLineSource *sourcePtr = source.get();

    LogConfiguration configuration;
    configuration.columns.push_back(
        Column{
            .header = "kind",
            .keys = {"kind"},
            .printFormat = "{}",
            .type = ColumnType::Enumeration,
            .parseFormats = {},
            .autoDetect = false,
        }
    );
    LogTable table(std::move(configuration));
    table.SetEnumValueCap(4);
    table.BeginStreaming(std::move(source));

    const auto rowsOf = [](const std::vector<std::string> &values) {
        std::vector<Fields> rows;
        rows.reserve(values.size());
        for (const std::string &value : values)
        {
            rows.push_back({{"kind", value}});
        }
        return rows;
    };
    AppendFixtureBatch(table, MakeBatch(table.Keys(), *sourcePtr, rowsOf({"a", "b", "c", "d"}), 1));
    // "e" and "f" do not fit. The known values after them must still be encoded.
    AppendFixtureBatch(table, MakeBatch(table.Keys(), *sourcePtr, rowsOf({"a", "e", "b", "f", "c", "d"}), 5));

    REQUIRE(table.RowCount() == 10);
    CHECK(table.Configuration().Configuration().columns[0].type == ColumnType::Enumeration);
    for (const size_t row : {size_t{4}, size_t{6}, size_t{8}, size_t{9}})
    {
        INFO("row " << row);
        CHECK(table.GetEnumValueId(row, 0).has_value());
    }
    for (const size_t row : {size_t{5}, size_t{7}})
    {
        INFO("row " << row);
        CHECK_FALSE(table.GetEnumValueId(row, 0).has_value());
    }
    CHECK(table.GetFormattedValue(5, 0) == "e");
    CHECK(table.GetFormattedValue(8, 0) == "c");
}

namespace
{

// The column types after feeding the same rows in pieces of the given size. The key late appears from lateFrom on.
std::vector<ColumnType> TypesAfterSplit(
    const std::string &name, bool streaming, size_t rowCount, size_t lateFrom, size_t piece
)
{
    Stream stream(name, streaming);
    size_t next = 0;
    while (next < rowCount)
    {
        const size_t end = std::min(rowCount, next + piece);
        static_cast<void>(stream.Append([&](KeyIndex &keys, LineSource &source) {
            std::vector<Fields> rows;
            for (size_t i = next; i < end; ++i)
            {
                Fields fields{{"count", static_cast<int64_t>(i)}, {"tag", std::string(i % 3 == 0 ? "a" : "b")}};
                if (i >= lateFrom)
                {
                    fields.emplace_back("late", static_cast<int64_t>(i));
                }
                rows.push_back(std::move(fields));
            }
            return MakeBatch(keys, source, rows, next + 1);
        }));
        next = end;
    }
    static_cast<void>(stream.Finalize());
    std::vector<ColumnType> types;
    for (const Column &column : stream.Table().Configuration().Configuration().columns)
    {
        types.push_back(column.type);
    }
    return types;
}

} // namespace

TEST_CASE("Column types do not depend on how the rows are split across AppendBatch calls", "[append_report][types]")
{
    const bool streaming = GENERATE(true, false);
    const size_t rowCount = streaming ? 60 : 9000;
    const size_t lateFrom = streaming ? 20 : 1000;
    const std::string name = streaming ? "append_report_split_stream" : "append_report_split_static";

    const std::vector<ColumnType> whole = TypesAfterSplit(name, streaming, rowCount, lateFrom, rowCount);
    REQUIRE(whole.size() == 3);
    // count is the first key, tag the second, late the third.
    CHECK(whole[0] == ColumnType::Integer);
    CHECK(whole[2] == ColumnType::Integer);

    for (const size_t piece : {size_t{1}, size_t{2}, size_t{7}})
    {
        CAPTURE(streaming, piece);
        CHECK(TypesAfterSplit(name, streaming, rowCount, lateFrom, piece) == whole);
    }
}

TEST_CASE("A streamed column of all-distinct values does not stay an enumeration", "[append_report][types]")
{
    Stream stream("append_report_distinct_ids");
    static_cast<void>(stream.Append([](KeyIndex &keys, LineSource &source) {
        std::vector<Fields> rows;
        rows.reserve(30);
        for (size_t i = 0; i < 30; ++i)
        {
            rows.push_back({{"id", "id-" + std::to_string(i)}, {"status", std::string(i % 3 == 0 ? "ok" : "bad")}});
        }
        return MakeBatch(keys, source, rows, 1);
    }));
    const LogTable::FinalizeResult result = stream.Table().FinalizeAutoDetection();

    const auto &columns = stream.Table().Configuration().Configuration().columns;
    REQUIRE(columns.size() == 2);
    CHECK(columns[0].type == ColumnType::String);
    CHECK(columns[1].type == ColumnType::Enumeration);
    CHECK(std::ranges::any_of(result.enumEvents, [](const EnumColumnEvent &event) {
        return event.column == 0 && event.change == EnumColumnChange::Demoted;
    }));
}
