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
// The reference: the snapshot and diff that LogModel runs around AppendBatch and in EndStreaming, with plain vectors
// instead of signals. It uses only the old members of the table, so it is independent of the report it is compared
// with.
// ---------------------------------------------------------------------------------------------------------------------

struct ReferenceEvent
{
    size_t column = 0;
    EnumColumnChange change = EnumColumnChange::Grew;
    bool causedByBackfill = false;
    LevelValueMap levelValues;
};

struct ReferenceOutcome
{
    std::vector<ReferenceEvent> events;
    std::vector<ColumnMove> moves;
};

bool IsEnumLikeType(ColumnType type)
{
    return type == ColumnType::Enumeration || type == ColumnType::Level;
}

LevelValueMap ReferenceLevelValues(const LogTable &table, size_t columnIndex, const EnumDictionary &dictionary)
{
    LevelValueMap values;
    const std::vector<LogLevel> *ranks = table.LevelRankCache(columnIndex);
    if (ranks == nullptr)
    {
        return values;
    }
    const size_t resolveCount = std::min<size_t>(ranks->size(), dictionary.Size());
    for (size_t valueId = 0; valueId < resolveCount; ++valueId)
    {
        if ((*ranks)[valueId] != LogLevel::Unknown)
        {
            values[(*ranks)[valueId]].emplace_back(dictionary.Resolve(static_cast<EnumValueId>(valueId)));
        }
    }
    return values;
}

ReferenceOutcome ReferenceAppend(LogTable &table, RowBatch batch)
{
    ReferenceOutcome outcome;
    const size_t oldColumnCount = table.ColumnCount();

    struct Entry
    {
        KeyId kid;
        size_t columnIndex;
        uint16_t sizeBefore;
        ColumnType typeBefore;
        LevelValueMap levelValues;
    };
    std::vector<Entry> snapshot;
    {
        const auto &columns = table.Configuration().Configuration().columns;
        for (size_t columnIndex = 0; columnIndex < columns.size(); ++columnIndex)
        {
            const Column &column = columns[columnIndex];
            if (!IsEnumLikeType(column.type) || column.keys.empty())
            {
                continue;
            }
            const KeyId kid = table.Keys().Find(column.keys.front());
            if (kid == INVALID_KEY_ID)
            {
                continue;
            }
            const EnumDictionary *dictionary = table.EnumDictionaries().Find(kid);
            if (dictionary == nullptr)
            {
                continue;
            }
            Entry entry{
                .kid = kid,
                .columnIndex = columnIndex,
                .sizeBefore = dictionary->Size(),
                .typeBefore = column.type,
                .levelValues = {},
            };
            if (column.type == ColumnType::Level)
            {
                entry.levelValues = ReferenceLevelValues(table, columnIndex, *dictionary);
            }
            snapshot.push_back(std::move(entry));
        }
    }

    const LogTable::AppendBatchResult result = table.AppendBatch(std::move(batch));

    std::vector<size_t> demotedColumns;
    const auto &columnsAfter = table.Configuration().Configuration().columns;
    for (Entry &entry : snapshot)
    {
        const EnumDictionary *dictionary = table.EnumDictionaries().Find(entry.kid);
        if (dictionary == nullptr)
        {
            demotedColumns.push_back(entry.columnIndex);
            ReferenceEvent event{.column = entry.columnIndex, .change = EnumColumnChange::Demoted};
            if (entry.typeBefore == ColumnType::Level && !entry.levelValues.empty())
            {
                event.levelValues = std::move(entry.levelValues);
            }
            outcome.events.push_back(std::move(event));
            continue;
        }
        const ColumnType typeAfter =
            entry.columnIndex < columnsAfter.size() ? columnsAfter[entry.columnIndex].type : entry.typeBefore;
        if (entry.typeBefore == ColumnType::Enumeration && typeAfter == ColumnType::Level)
        {
            outcome.events.push_back({.column = entry.columnIndex, .change = EnumColumnChange::Promoted});
        }
        else if (dictionary->Size() != entry.sizeBefore)
        {
            outcome.events.push_back({.column = entry.columnIndex, .change = EnumColumnChange::Grew});
        }
    }
    for (const KeyId kid : result.demotedKeys)
    {
        const int columnIndex = table.FindColumnIndexByKey(kid);
        if (columnIndex < 0 ||
            std::ranges::find(demotedColumns, static_cast<size_t>(columnIndex)) != demotedColumns.end())
        {
            continue;
        }
        demotedColumns.push_back(static_cast<size_t>(columnIndex));
        outcome.events.push_back({.column = static_cast<size_t>(columnIndex), .change = EnumColumnChange::Demoted});
    }
    if (result.backfilledColumns.has_value() && table.RowCount() > 0)
    {
        for (size_t columnIndex = result.backfilledColumns->first; columnIndex <= result.backfilledColumns->second;
             ++columnIndex)
        {
            if (columnIndex >= columnsAfter.size() || !IsEnumLikeType(columnsAfter[columnIndex].type))
            {
                continue;
            }
            if (std::ranges::any_of(snapshot, [columnIndex](const Entry &e) { return e.columnIndex == columnIndex; }))
            {
                continue;
            }
            outcome.events.push_back(
                {.column = columnIndex, .change = EnumColumnChange::Promoted, .causedByBackfill = true}
            );
        }
    }

    for (const size_t source : table.TimeColumnsToFront(oldColumnCount))
    {
        outcome.moves.push_back({.from = source, .to = 0});
        table.MoveColumn(source, 0);
    }
    for (const KeyId kid : result.levelBubbleKeys)
    {
        const int source = table.FindColumnIndexByKey(kid);
        if (source < 0 || !ShouldBubbleLevelColumn(table.Configuration().Configuration(), static_cast<size_t>(source)))
        {
            continue;
        }
        outcome.moves.push_back({.from = static_cast<size_t>(source), .to = CANONICAL_LEVEL_COLUMN_INDEX});
        table.MoveColumn(static_cast<size_t>(source), CANONICAL_LEVEL_COLUMN_INDEX);
    }
    return outcome;
}

ReferenceOutcome ReferenceFinalize(LogTable &table)
{
    ReferenceOutcome outcome;
    std::vector<ColumnType> typesBefore;
    std::vector<std::pair<size_t, LevelValueMap>> levelValuesBefore;
    {
        const auto &columns = table.Configuration().Configuration().columns;
        for (size_t i = 0; i < columns.size(); ++i)
        {
            typesBefore.push_back(columns[i].type);
            if (columns[i].type != ColumnType::Level || columns[i].keys.empty())
            {
                continue;
            }
            const KeyId kid = table.Keys().Find(columns[i].keys.front());
            const EnumDictionary *dictionary = kid == INVALID_KEY_ID ? nullptr : table.EnumDictionaries().Find(kid);
            if (dictionary == nullptr)
            {
                continue;
            }
            LevelValueMap values = ReferenceLevelValues(table, i, *dictionary);
            if (!values.empty())
            {
                levelValuesBefore.emplace_back(i, std::move(values));
            }
        }
    }

    const LogTable::FinalizeResult finalized = table.FinalizeAutoDetection();

    const auto &columnsAfter = table.Configuration().Configuration().columns;
    for (size_t i = 0; i < std::min(typesBefore.size(), columnsAfter.size()); ++i)
    {
        const ColumnType before = typesBefore[i];
        const ColumnType after = columnsAfter[i].type;
        const auto mappingOf = [&](size_t column) {
            for (auto &[index, values] : levelValuesBefore)
            {
                if (index == column)
                {
                    return values;
                }
            }
            return LevelValueMap{};
        };
        if ((!IsEnumLikeType(before) && IsEnumLikeType(after)) ||
            (before == ColumnType::Enumeration && after == ColumnType::Level))
        {
            outcome.events.push_back({.column = i, .change = EnumColumnChange::Promoted});
        }
        else if (
            (IsEnumLikeType(before) && !IsEnumLikeType(after)) ||
            (before == ColumnType::Level && after == ColumnType::Enumeration)
        )
        {
            outcome.events.push_back({.column = i, .change = EnumColumnChange::Demoted, .levelValues = mappingOf(i)});
        }
    }
    for (const KeyId kid : finalized.levelBubbleKeys)
    {
        const int source = table.FindColumnIndexByKey(kid);
        if (source < 0 || !ShouldBubbleLevelColumn(table.Configuration().Configuration(), static_cast<size_t>(source)))
        {
            continue;
        }
        outcome.moves.push_back({.from = static_cast<size_t>(source), .to = CANONICAL_LEVEL_COLUMN_INDEX});
        table.MoveColumn(static_cast<size_t>(source), CANONICAL_LEVEL_COLUMN_INDEX);
    }
    return outcome;
}

// ---------------------------------------------------------------------------------------------------------------------
// Two tables fed the same batches. One goes through the reference, the other through the report.
// ---------------------------------------------------------------------------------------------------------------------

using BatchMaker = std::function<RowBatch(KeyIndex &, LineSource &)>;

class TablePair
{
public:
    explicit TablePair(const std::string &name, bool streaming = true, uint16_t enumCap = 0)
        : mFileA(name + "_ref.json"), mFileB(name + "_report.json")
    {
        InitializeTimezoneData();
        mFileA.Write("");
        mFileB.Write("");
        Start(mReference, mFileA, mSourceA, streaming, enumCap);
        Start(mReport, mFileB, mSourceB, streaming, enumCap);
    }

    void Append(const BatchMaker &make)
    {
        const ReferenceOutcome expected = ReferenceAppend(mReference, make(mReference.Keys(), *mSourceA));
        const LogTable::AppendBatchResult result = mReport.AppendBatch(make(mReport.Keys(), *mSourceB));
        Compare(expected, result.enumEvents, result.placements);
        CHECK(result.errors.empty());
    }

    void Finalize()
    {
        const ReferenceOutcome expected = ReferenceFinalize(mReference);
        const LogTable::FinalizeResult result = mReport.FinalizeAutoDetection();
        Compare(expected, result.enumEvents, result.placements);
    }

    // Everything the report said, so a scenario can check that it exercised what it meant to.
    [[nodiscard]] const std::vector<EnumColumnEvent> &Events() const noexcept
    {
        return mAllEvents;
    }

    [[nodiscard]] const std::vector<ColumnMove> &Moves() const noexcept
    {
        return mAllMoves;
    }

    [[nodiscard]] LogTable &Report() noexcept
    {
        return mReport;
    }

    void ExpectSameLayout() const
    {
        REQUIRE(mReference.RowCount() == mReport.RowCount());
        REQUIRE(mReference.ColumnCount() == mReport.ColumnCount());
        const auto &expected = mReference.Configuration().Configuration().columns;
        const auto &actual = mReport.Configuration().Configuration().columns;
        for (size_t i = 0; i < expected.size(); ++i)
        {
            CHECK(expected[i].header == actual[i].header);
            CHECK(expected[i].keys == actual[i].keys);
            CHECK(expected[i].type == actual[i].type);
        }
    }

private:
    static void Start(
        LogTable &table, const TestLogFile &file, FileLineSource *&sourcePtr, bool streaming, uint16_t cap
    )
    {
        auto source = std::make_unique<FileLineSource>(file.GetFilePath());
        sourcePtr = source.get();
        if (cap != 0)
        {
            table.SetEnumValueCap(cap);
        }
        if (streaming)
        {
            table.BeginStreaming(std::move(source));
        }
        else
        {
            table.AppendStreaming(std::move(source));
        }
    }

    void Compare(
        const ReferenceOutcome &expected,
        const std::vector<EnumColumnEvent> &events,
        const std::vector<ColumnPlacement> &placements
    )
    {
        REQUIRE(events.size() == expected.events.size());
        for (size_t i = 0; i < events.size(); ++i)
        {
            INFO("event " << i);
            CHECK(events[i].column == expected.events[i].column);
            CHECK(events[i].change == expected.events[i].change);
            CHECK(events[i].causedByBackfill == expected.events[i].causedByBackfill);
            CHECK(events[i].levelValuesBeforeDemotion == expected.events[i].levelValues);
            mAllEvents.push_back(events[i]);
        }
        std::vector<ColumnMove> moves;
        for (const ColumnPlacement &placement : placements)
        {
            if (const auto move = mReport.ResolvePlacement(placement); move.has_value())
            {
                moves.push_back(*move);
                mReport.MoveColumn(move->from, move->to);
            }
        }
        REQUIRE(moves.size() == expected.moves.size());
        for (size_t i = 0; i < moves.size(); ++i)
        {
            CHECK(moves[i].from == expected.moves[i].from);
            CHECK(moves[i].to == expected.moves[i].to);
            mAllMoves.push_back(moves[i]);
        }
        ExpectSameLayout();
    }

    TestLogFile mFileA;
    TestLogFile mFileB;
    FileLineSource *mSourceA = nullptr;
    FileLineSource *mSourceB = nullptr;
    LogTable mReference;
    LogTable mReport;
    std::vector<EnumColumnEvent> mAllEvents;
    std::vector<ColumnMove> mAllMoves;
};

bool HasEvent(const std::vector<EnumColumnEvent> &events, EnumColumnChange change, bool backfill = false)
{
    return std::ranges::any_of(events, [&](const EnumColumnEvent &event) {
        return event.change == change && event.causedByBackfill == backfill;
    });
}

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

TEST_CASE(
    "The append report equals the GUI rules for a static-like load in any batch size", "[append_report][reference]"
)
{
    const size_t batchSize = GENERATE(size_t{1}, size_t{7}, size_t{4096});
    constexpr size_t TOTAL_ROWS = 4200;
    const std::vector<std::string> levels = {"info", "warn", "error", "debug"};

    TablePair pair("append_report_static_" + std::to_string(batchSize));
    for (size_t first = 0; first < TOTAL_ROWS; first += batchSize)
    {
        const size_t last = std::min(first + batchSize, TOTAL_ROWS);
        pair.Append([&](KeyIndex &keys, LineSource &source) {
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
        });
    }
    pair.Finalize();
    pair.ExpectSameLayout();

    const auto &columns = pair.Report().Configuration().Configuration().columns;
    REQUIRE(columns.size() == 4);
    CHECK(columns[0].header == "timestamp");
    CHECK(columns[1].header == "level");
    CHECK(columns[1].type == ColumnType::Level);
    CHECK(HasEvent(pair.Events(), EnumColumnChange::Promoted, true));
    CHECK_FALSE(pair.Moves().empty());
}

TEST_CASE("The append report equals the GUI rules when a column grows and then demotes", "[append_report][reference]")
{
    TablePair pair("append_report_grow_demote", true, 4);
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
    pair.Append(kinds({"a", "b", "a"}, 1));
    pair.Append(kinds({"c"}, 4));
    pair.Append(kinds({"d"}, 5));
    pair.Append(kinds({"e", "f"}, 6));
    pair.Finalize();

    CHECK(HasEvent(pair.Events(), EnumColumnChange::Promoted, true));
    CHECK(HasEvent(pair.Events(), EnumColumnChange::Grew));
    CHECK(HasEvent(pair.Events(), EnumColumnChange::Demoted));
    CHECK(pair.Report().Configuration().Configuration().columns[0].type == ColumnType::String);
}

TEST_CASE(
    "The append report equals the GUI rules for Unknown to Enumeration to String in one batch",
    "[append_report][reference]"
)
{
    TablePair pair("append_report_same_batch", true, 2);
    pair.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(
            keys, source, {{{"kind", std::string("a")}}, {{"kind", std::string("b")}}, {{"kind", std::string("c")}}}, 1
        );
    });
    pair.Finalize();

    REQUIRE(pair.Events().size() == 1);
    CHECK(pair.Events().front().change == EnumColumnChange::Demoted);
    CHECK(pair.Events().front().column == 0);
    CHECK(pair.Report().Configuration().Configuration().columns[0].type == ColumnType::String);
}

TEST_CASE("The append report carries the level values of a demoted Level column", "[append_report][reference]")
{
    TablePair pair("append_report_level_demote", true, 4);
    pair.Append([](KeyIndex &keys, LineSource &source) {
        std::vector<Fields> rows;
        for (const char *level : {"info", "warn", "error", "info", "warn", "error"})
        {
            rows.push_back({{"msg", std::string("m")}, {"level", std::string(level)}});
        }
        return MakeBatch(keys, source, rows, 1);
    });
    pair.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(keys, source, {{{"msg", std::string("m")}, {"level", std::string("x1")}}}, 7);
    });
    pair.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(
            keys,
            source,
            {
                {{"msg", std::string("m")}, {"level", std::string("x2")}},
                {{"msg", std::string("m")}, {"level", std::string("x3")}},
            },
            8
        );
    });
    pair.Finalize();

    const auto demoted = std::ranges::find_if(pair.Events(), [](const EnumColumnEvent &event) {
        return event.change == EnumColumnChange::Demoted;
    });
    REQUIRE(demoted != pair.Events().end());
    REQUIRE(demoted->levelValuesBeforeDemotion.size() == 3);
    CHECK(demoted->levelValuesBeforeDemotion.at(LogLevel::Info) == std::vector<std::string>{"info"});
    CHECK(demoted->levelValuesBeforeDemotion.at(LogLevel::Warn) == std::vector<std::string>{"warn"});
    CHECK(demoted->levelValuesBeforeDemotion.at(LogLevel::Error) == std::vector<std::string>{"error"});
}

TEST_CASE(
    "The append report keeps the first of two Level-like columns in the canonical slot", "[append_report][reference]"
)
{
    TablePair pair("append_report_two_levels");
    pair.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(keys, source, {{{"body", std::string("hello")}}}, 1);
    });
    pair.Append([](KeyIndex &keys, LineSource &source) {
        std::vector<Fields> rows;
        for (const char *level : {"info", "warn", "error", "info", "warn", "error"})
        {
            rows.push_back({{"level", std::string(level)}, {"severity", std::string(level)}});
        }
        return MakeBatch(keys, source, rows, 2);
    });
    pair.Finalize();

    const auto &columns = pair.Report().Configuration().Configuration().columns;
    REQUIRE(columns.size() == 3);
    CHECK(columns[0].header == "body");
    CHECK(columns[1].header == "level");
    CHECK(columns[2].header == "severity");
    CHECK(columns[1].type == ColumnType::Level);
    CHECK(columns[2].type == ColumnType::Level);
}

TEST_CASE(
    "The append report moves two new time columns of one batch to the front in order", "[append_report][reference]"
)
{
    TablePair pair("append_report_two_times");
    pair.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(keys, source, {{{"msg", std::string("first")}}}, 1);
    });
    pair.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(
            keys, source, {{{"msg", std::string("second")}, {"timestamp", TimeAt(1)}, {"time", TimeAt(2)}}}, 2
        );
    });
    pair.Finalize();

    const auto &columns = pair.Report().Configuration().Configuration().columns;
    REQUIRE(columns.size() == 3);
    CHECK(columns[0].type == ColumnType::Time);
    CHECK(columns[1].type == ColumnType::Time);
    CHECK(columns[2].header == "msg");
    CHECK(pair.Moves().size() == 2);
}

TEST_CASE(
    "The append report equals the GUI rules for promotions and demotions at the end of a stream",
    "[append_report][reference]"
)
{
    SECTION("an undecided column is promoted when the stream ends")
    {
        TablePair pair("append_report_finalize_promote", false);
        pair.Append([](KeyIndex &keys, LineSource &source) {
            std::vector<Fields> rows;
            for (const char *level : {"info", "warn", "info", "warn"})
            {
                rows.push_back(
                    {{"msg", std::string("m")}, {"level", std::string(level)}, {"tier", std::string(level)}}
                );
            }
            return MakeBatch(keys, source, rows, 1);
        });
        CHECK(pair.Events().empty());
        pair.Finalize();
        CHECK(HasEvent(pair.Events(), EnumColumnChange::Promoted));
        CHECK(pair.Report().Configuration().Configuration().columns[1].type == ColumnType::Level);
    }

    SECTION("a Level column that fails its health check is demoted with its level values")
    {
        TablePair pair("append_report_finalize_demote", true);
        pair.Append([](KeyIndex &keys, LineSource &source) {
            std::vector<Fields> rows;
            for (const char *level : {"info", "warn", "error", "info", "warn", "error"})
            {
                rows.push_back({{"msg", std::string("m")}, {"level", std::string(level)}});
            }
            return MakeBatch(keys, source, rows, 1);
        });
        pair.Append([](KeyIndex &keys, LineSource &source) {
            std::vector<Fields> rows;
            rows.reserve(10);
            for (int i = 0; i < 10; ++i)
            {
                rows.push_back({{"msg", std::string("m")}, {"level", std::string(80, 'q')}});
            }
            return MakeBatch(keys, source, rows, 7);
        });
        pair.Finalize();

        const auto demoted = std::ranges::find_if(pair.Events(), [](const EnumColumnEvent &event) {
            return event.change == EnumColumnChange::Demoted;
        });
        REQUIRE(demoted != pair.Events().end());
        CHECK(demoted->levelValuesBeforeDemotion.size() == 3);
    }
}

TEST_CASE("ResolvePlacement gives nothing for a missing or already placed column", "[append_report][placement]")
{
    TablePair pair("append_report_placement");
    pair.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(keys, source, {{{"msg", std::string("first")}}}, 1);
    });
    pair.Append([](KeyIndex &keys, LineSource &source) {
        return MakeBatch(keys, source, {{{"msg", std::string("second")}, {"timestamp", TimeAt(1)}}}, 2);
    });
    LogTable &table = pair.Report();
    const KeyId timeKey = table.Keys().Find("timestamp");
    const KeyId msgKey = table.Keys().Find("msg");
    REQUIRE(timeKey != INVALID_KEY_ID);
    REQUIRE(table.GetHeader(0) == "timestamp");

    // Already at the front, and a column that does not exist.
    CHECK_FALSE(table.ResolvePlacement({.kind = ColumnPlacement::Kind::TimeToFront, .key = timeKey}).has_value());
    CHECK_FALSE(
        table.ResolvePlacement({.kind = ColumnPlacement::Kind::TimeToFront, .key = INVALID_KEY_ID}).has_value()
    );
    CHECK_FALSE(
        table.ResolvePlacement({.kind = ColumnPlacement::Kind::LevelToCanonicalSlot, .key = KeyId{999}}).has_value()
    );

    // A column that is not at the front gets a move to it.
    const auto toFront = table.ResolvePlacement({.kind = ColumnPlacement::Kind::TimeToFront, .key = msgKey});
    REQUIRE(toFront.has_value());
    CHECK(toFront->from == 1);
    CHECK(toFront->to == 0);

    // The canonical slot is taken by the column itself.
    CHECK_FALSE(
        table.ResolvePlacement({.kind = ColumnPlacement::Kind::LevelToCanonicalSlot, .key = msgKey}).has_value()
    );
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
