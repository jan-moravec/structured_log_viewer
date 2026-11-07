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

/// A batch of the given rows. Keys that the index did not hold before the call become newKeys, in id order.
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

} // namespace

TEST_CASE("Level rank cache is found when the first configured key appears after an alias", "[append_report][c20]")
{
    const TestLogFile testFile("append_report_c20.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(testFile.GetFilePath());
    FileLineSource *sourcePtr = source.get();

    LogConfiguration configuration;
    configuration.columns.push_back(
        Column{
            .header = "level",
            .keys = {"level", "severity"},
            .printFormat = "{}",
            .type = ColumnType::Level,
            .parseFormats = {}
        }
    );
    LogTable table(std::move(configuration));
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
