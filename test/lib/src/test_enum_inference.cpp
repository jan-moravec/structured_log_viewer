#include <loglib/enum_inference.hpp>
#include <loglib/file_line_source.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
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

TEST_CASE("RouteNoStringBail picks a terminal type from tag counts", "[enum_inference]")
{
    CHECK(RouteNoStringBail(0, 0, 0, 0) == ColumnType::Any);
    CHECK(RouteNoStringBail(0, 0, 0, 4) == ColumnType::Boolean);
    CHECK(RouteNoStringBail(3, 0, 0, 0) == ColumnType::Integer);
    CHECK(RouteNoStringBail(0, 2, 0, 0) == ColumnType::Integer);
    CHECK(RouteNoStringBail(0, 0, 5, 0) == ColumnType::Floating);
    CHECK(RouteNoStringBail(1, 0, 1, 0) == ColumnType::Number);
    CHECK(RouteNoStringBail(1, 0, 0, 1) == ColumnType::Any);
}

TEST_CASE("IsEnumPassEligible covers enum, level, and auto-detect Any", "[enum_inference]")
{
    const Column anyAuto{
        .header = "x", .keys = {"x"}, .printFormat = "{}", .type = ColumnType::Any, .autoDetect = true
    };
    const Column anyPinned{
        .header = "x", .keys = {"x"}, .printFormat = "{}", .type = ColumnType::Any, .autoDetect = false
    };
    const Column enumeration{.header = "x", .keys = {"x"}, .printFormat = "{}", .type = ColumnType::Enumeration};
    const Column level{.header = "x", .keys = {"x"}, .printFormat = "{}", .type = ColumnType::Level};
    const Column integer{.header = "x", .keys = {"x"}, .printFormat = "{}", .type = ColumnType::Integer};

    CHECK(IsEnumPassEligible(anyAuto));
    CHECK_FALSE(IsEnumPassEligible(anyPinned));
    CHECK(IsEnumPassEligible(enumeration));
    CHECK(IsEnumPassEligible(level));
    CHECK_FALSE(IsEnumPassEligible(integer));
}

TEST_CASE("EnumCandidateTracker::Observe caps distinct values and long strings", "[enum_inference]")
{
    EnumCandidateTracker tracker{2, 8};
    tracker.presenceCount = 1;
    tracker.Observe("ok");
    CHECK(tracker.size == 1);
    CHECK(tracker.seen.contains(std::string_view{"ok"}));

    tracker.presenceCount = 2;
    tracker.Observe("ok");
    CHECK(tracker.size == 1);

    tracker.presenceCount = 3;
    tracker.Observe("also");
    CHECK(tracker.size == 2);
    CHECK_FALSE(tracker.killed);

    tracker.presenceCount = 4;
    tracker.Observe("third");
    CHECK(tracker.killed);
    CHECK(tracker.size == 0);
    CHECK(tracker.values.empty());
}

TEST_CASE("EnumCandidateTracker::Observe accrues long values until the health budget kills", "[enum_inference]")
{
    EnumCandidateTracker tracker{8, 4};
    for (size_t i = 0; i < ENUM_HEALTH_MIN_SAMPLES; ++i)
    {
        ++tracker.presenceCount;
        tracker.Observe("toolong");
    }
    CHECK(tracker.killed);
    CHECK(tracker.longValueCount >= ENUM_HEALTH_MIN_SAMPLES);
}

TEST_CASE("EnumColumnHealth::ShouldDemote waits for minSamples then applies the ratio", "[enum_inference]")
{
    EnumColumnHealth health{.totalSlots = 10, .longValueSlots = 1, .wrongTypeSlots = 0};
    CHECK_FALSE(health.ShouldDemote(0.01, 50));
    health.totalSlots = 100;
    health.longValueSlots = 2;
    CHECK(health.ShouldDemote(0.01, 50));
    health.longValueSlots = 0;
    CHECK_FALSE(health.ShouldDemote(0.01, 50));
}

TEST_CASE("EnumInference encodes a pinned Enumeration column as DictRef", "[enum_inference]")
{
    const TestLogFile testFile("enum_inference_encode.json");
    testFile.Write("a\nb\n");
    auto source = std::make_unique<FileLineSource>(std::make_unique<LogFile>(testFile.GetFilePath()));
    LineSource &sourceRef = *source;

    KeyIndex keys;
    std::vector<LogLine> lines;
    auto makeLine = [&](const std::string &value) {
        std::vector<std::pair<KeyId, LogValue>> sorted;
        sorted.emplace_back(keys.GetOrInsert("status"), LogValue{value});
        return LogLine(std::move(sorted), keys, sourceRef, lines.size());
    };
    lines.push_back(makeLine("ok"));
    lines.push_back(makeLine("fail"));

    LogData data(std::move(source), std::move(lines), std::move(keys));

    LogConfiguration configuration;
    configuration.columns.push_back(
        Column{
            .header = "status",
            .keys = {"status"},
            .printFormat = "{}",
            .type = ColumnType::Enumeration,
            .parseFormats = {},
            .visible = true,
            .levelMapping = {},
            .autoDetect = false,
        }
    );
    const TestLogConfiguration cfgFile("enum_inference_encode_cfg.json");
    cfgFile.Write(configuration);
    LogConfigurationManager manager;
    manager.Load(cfgFile.GetFilePath());

    EnumInference inference;
    inference.RefreshSnapshot(data, manager);
    EnumColumnHealth health;
    REQUIRE(
        inference.EncodeColumnRangeAsEnum(data, manager.Configuration().columns[0], 0, data.Lines().size(), health)
    );
    CHECK(health.totalSlots == 2);
    CHECK(health.longValueSlots == 0);
    CHECK(health.wrongTypeSlots == 0);

    const KeyId status = data.Keys().Find("status");
    REQUIRE(status != INVALID_KEY_ID);
    const EnumDictionary *dict = inference.Dictionaries().Find(status);
    REQUIRE(dict != nullptr);
    CHECK(dict->Size() == 2);

    const CompactLogValue *slot0 = data.Lines()[0].FindCompact(status);
    REQUIRE(slot0 != nullptr);
    CHECK(slot0->tag == CompactTag::DictRef);
}
