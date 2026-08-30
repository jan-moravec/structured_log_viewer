#include <loglib/column_schema.hpp>
#include <loglib/column_type_health.hpp>
#include <loglib/compact_log_value.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_value.hpp>

#include "common.hpp"

#include <catch2/catch_all.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace loglib;

TEST_CASE("TagMatchesType rejects monostate for every declared type", "[column_type_health]")
{
    for (const ColumnType type :
         {ColumnType::Any,
          ColumnType::String,
          ColumnType::Boolean,
          ColumnType::Integer,
          ColumnType::Floating,
          ColumnType::Number,
          ColumnType::Time,
          ColumnType::Enumeration,
          ColumnType::Level})
    {
        CHECK_FALSE(TagMatchesType(CompactTag::Monostate, type));
    }
}

TEST_CASE("TagMatchesType accepts the comparators' native tags", "[column_type_health]")
{
    CHECK(TagMatchesType(CompactTag::OwnedString, ColumnType::Any));
    CHECK(TagMatchesType(CompactTag::Int64, ColumnType::Any));

    CHECK(TagMatchesType(CompactTag::MmapSlice, ColumnType::String));
    CHECK(TagMatchesType(CompactTag::OwnedString, ColumnType::String));
    CHECK(TagMatchesType(CompactTag::DictRef, ColumnType::String));
    CHECK_FALSE(TagMatchesType(CompactTag::Int64, ColumnType::String));

    CHECK(TagMatchesType(CompactTag::Bool, ColumnType::Boolean));
    CHECK_FALSE(TagMatchesType(CompactTag::OwnedString, ColumnType::Boolean));

    CHECK(TagMatchesType(CompactTag::Int64, ColumnType::Integer));
    CHECK(TagMatchesType(CompactTag::Uint64, ColumnType::Integer));
    CHECK_FALSE(TagMatchesType(CompactTag::Double, ColumnType::Integer));

    CHECK(TagMatchesType(CompactTag::Double, ColumnType::Floating));
    CHECK_FALSE(TagMatchesType(CompactTag::Int64, ColumnType::Floating));

    CHECK(TagMatchesType(CompactTag::Int64, ColumnType::Number));
    CHECK(TagMatchesType(CompactTag::Double, ColumnType::Number));

    CHECK(TagMatchesType(CompactTag::Timestamp, ColumnType::Time));
    CHECK(TagMatchesType(CompactTag::Int64, ColumnType::Time));
    CHECK_FALSE(TagMatchesType(CompactTag::OwnedString, ColumnType::Time));

    CHECK(TagMatchesType(CompactTag::DictRef, ColumnType::Enumeration));
    CHECK_FALSE(TagMatchesType(CompactTag::OwnedString, ColumnType::Enumeration));
    CHECK(TagMatchesType(CompactTag::DictRef, ColumnType::Level));
    CHECK_FALSE(TagMatchesType(CompactTag::OwnedString, ColumnType::Level));
}

TEST_CASE("ComputeColumnTypeHealth is a stateless walk of borrowed rows", "[column_type_health]")
{
    const TestLogFile testFile("column_type_health_direct.json");
    testFile.Write("");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    LineSource &sourceRef = *source;

    KeyIndex keys;
    std::vector<LogLine> lines;
    auto makeLine = [&](std::vector<std::pair<std::string, LogValue>> fields) {
        std::vector<std::pair<KeyId, LogValue>> sorted;
        for (auto &field : fields)
        {
            sorted.emplace_back(keys.GetOrInsert(field.first), std::move(field.second));
        }
        std::ranges::sort(sorted, [](const auto &a, const auto &b) { return a.first < b.first; });
        return LogLine(std::move(sorted), keys, sourceRef, lines.size());
    };

    lines.push_back(makeLine({{"value", LogValue{static_cast<int64_t>(1)}}}));
    lines.push_back(makeLine({{"value", LogValue{static_cast<int64_t>(2)}}}));
    lines.push_back(makeLine({{"value", LogValue{std::string("x")}}}));
    lines.push_back(makeLine({{"other", LogValue{static_cast<int64_t>(9)}}}));

    Column column{
        .header = "value",
        .keys = {"value"},
        .printFormat = "{}",
        .type = ColumnType::Integer,
        .parseFormats = {},
        .visible = true,
        .levelMapping = {},
        .autoDetect = false,
    };

    const auto health = ComputeColumnTypeHealth(lines, keys, column);
    CHECK(health.totalSlots == 4);
    CHECK(health.presentSlots == 3);
    CHECK(health.matchingSlots == 2);

    Column keyless = column;
    keyless.keys.clear();
    const auto absentKeys = ComputeColumnTypeHealth(lines, keys, keyless);
    CHECK(absentKeys.totalSlots == 4);
    CHECK(absentKeys.presentSlots == 0);
    CHECK(absentKeys.matchingSlots == 0);
}
