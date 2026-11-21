#include "common.hpp"
#include "log_value_helpers.hpp"

#include <loglib/core/compact_log_value.hpp>
#include <loglib/core/enum_dictionary.hpp>
#include <loglib/core/internal/memory_stats.hpp>
#include <loglib/core/key_index.hpp>
#include <loglib/core/log_line.hpp>

#include <test_common/log_line_debug.hpp>

#include <catch2/catch_all.hpp>

#include <chrono>

using namespace loglib;

TEST_CASE("Construct LogLine with valid values and file reference", "[log_line]")
{
    const test_common::LogMap map{
        {"key1", std::string("value1")},
        {"key2", static_cast<uint64_t>(42)},
        {"key3", static_cast<int64_t>(-12)},
        {"key4", 3.14},
        {"key5", true},
        {"key6", std::monostate()},
        {"key7", TimeStamp{std::chrono::microseconds{1'714'298'402'123'456}}},
    };

    const TestLogFile testLogFile;
    testLogFile.Write("abcd\nefgh\n");
    auto source = testLogFile.CreateFileLineSource();

    KeyIndex keys;
    const LogLine line = test_common::MakeLine(map, keys, *source, 1);

    REQUIRE(test_common::ToMap(line, keys).size() == map.size());
    CHECK(std::get<std::string>(test_common::ValueByName(line, keys, "key1")) == "value1");
    CHECK(std::get<uint64_t>(test_common::ValueByName(line, keys, "key2")) == 42);
    CHECK(std::get<int64_t>(test_common::ValueByName(line, keys, "key3")) == -12);
    CHECK(std::get<double>(test_common::ValueByName(line, keys, "key4")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(test_common::ValueByName(line, keys, "key5")) == true);
    CHECK(std::holds_alternative<std::monostate>(test_common::ValueByName(line, keys, "key6")));
    CHECK(
        std::get<TimeStamp>(test_common::ValueByName(line, keys, "key7")) ==
        TimeStamp{std::chrono::microseconds{1'714'298'402'123'456}}
    );

    REQUIRE(line.Source() != nullptr);
    // `TestLogFile` resolves the fixture under a per-process scratch dir, so
    // compare against the fixture's own resolved path rather than the bare
    // filename literal.
    CHECK(line.Source()->Path() == testLogFile.GetFilePath());
    CHECK(line.LineId() == 1);
    CHECK(line.Source()->RawLine(line.LineId()) == "efgh");

    auto resultKeys = test_common::KeyNames(line, keys);
    REQUIRE(resultKeys.size() == map.size());
    CHECK(std::ranges::find(resultKeys, "key1") != resultKeys.end());
    CHECK(std::ranges::find(resultKeys, "key2") != resultKeys.end());
    CHECK(std::ranges::find(resultKeys, "key3") != resultKeys.end());
    CHECK(std::ranges::find(resultKeys, "key4") != resultKeys.end());
    CHECK(std::ranges::find(resultKeys, "key5") != resultKeys.end());
    CHECK(std::ranges::find(resultKeys, "key6") != resultKeys.end());
    CHECK(std::ranges::find(resultKeys, "key7") != resultKeys.end());

    const test_common::LogMap resultMap = test_common::ToMap(line, keys);

    REQUIRE(resultMap.size() == map.size());

    for (const auto &[key, expectedValue] : map)
    {
        REQUIRE(resultMap.count(key) == 1);

        std::visit(
            [&](const auto &expected) {
                using T = std::decay_t<decltype(expected)>;
                const auto &actual = std::get<T>(resultMap.at(key));

                if constexpr (std::is_same_v<T, double>)
                {
                    CHECK(actual == Catch::Approx(expected));
                }
                else
                {
                    CHECK(actual == expected);
                }
            },
            expectedValue
        );
    }
}

TEST_CASE("KeyNames and ToMap are empty for an empty LogLine", "[log_line]")
{
    const test_common::LogMap emptyMap;

    const TestLogFile testLogFile;
    auto source = testLogFile.CreateFileLineSource();

    KeyIndex keys;
    const LogLine emptyLine = test_common::MakeLine(emptyMap, keys, *source, 0);

    const auto resultKeys = test_common::KeyNames(emptyLine, keys);
    CHECK(resultKeys.empty());
    CHECK(test_common::ToMap(emptyLine, keys).empty());
}

TEST_CASE("LogLine returns monostate for empty and non-existent key", "[log_line]")
{
    const test_common::LogMap map{{"key1", std::string("value1")}, {"key2", static_cast<uint64_t>(42)}};

    const TestLogFile testLogFile;
    auto source = testLogFile.CreateFileLineSource();

    KeyIndex keys;
    const LogLine line = test_common::MakeLine(map, keys, *source, 0);

    CHECK(std::holds_alternative<std::monostate>(test_common::ValueByName(line, keys, "")));
    CHECK(std::holds_alternative<std::monostate>(test_common::ValueByName(line, keys, "non_existent_key")));

    CHECK(std::get<std::string>(test_common::ValueByName(line, keys, "key1")) == "value1");
    CHECK(std::get<uint64_t>(test_common::ValueByName(line, keys, "key2")) == 42);
}

TEST_CASE("Set and update values", "[log_line]")
{
    const test_common::LogMap map{{"existingKey", std::string("initialValue")}};

    const TestLogFile testLogFile;
    auto source = testLogFile.CreateFileLineSource();

    KeyIndex keys;
    LogLine line = test_common::MakeLine(map, keys, *source, 0);

    CHECK(std::get<std::string>(test_common::ValueByName(line, keys, "existingKey")) == "initialValue");
    CHECK(test_common::ToMap(line, keys).size() == 1);

    const std::string updatedValue = "updatedValue";
    line.SetValue(keys.Find("existingKey"), updatedValue);

    CHECK(std::get<std::string>(test_common::ValueByName(line, keys, "existingKey")) == updatedValue);
    CHECK(test_common::ToMap(line, keys).size() == 1);

    auto resultKeys = test_common::KeyNames(line, keys);
    REQUIRE(resultKeys.size() == 1);
    CHECK(resultKeys[0] == "existingKey");

    // A new field needs its key in the KeyIndex first.
    const std::string newKey = "newKey";
    const std::string newValue = "newValue";
    line.SetValue(keys.GetOrInsert(newKey), newValue);

    REQUIRE(test_common::ToMap(line, keys).size() == 2);
    CHECK(std::get<std::string>(test_common::ValueByName(line, keys, newKey)) == newValue);

    resultKeys = test_common::KeyNames(line, keys);
    REQUIRE(resultKeys.size() == 2);
    CHECK(std::ranges::find(resultKeys, newKey) != resultKeys.end());
}

// AsStringView reads text whether the parser stored a view or an owned string. HoldsString, ToOwnedLogValue and
// LogValueEquivalent are test helpers built on the same rule.
TEST_CASE("AsStringView returns bytes for both string alternatives, nullopt for non-strings", "[log_line][helpers]")
{
    const std::string owned = "owned-bytes";
    const std::string_view view = "view-bytes";

    const LogValue ownedValue{owned};
    const LogValue viewValue{view};
    const LogValue intValue{int64_t{42}};
    const LogValue boolValue{true};
    const LogValue monoValue{std::monostate{}};

    REQUIRE(AsStringView(ownedValue).has_value());
    CHECK(*AsStringView(ownedValue) == owned);

    REQUIRE(AsStringView(viewValue).has_value());
    CHECK(*AsStringView(viewValue) == view);

    CHECK_FALSE(AsStringView(intValue).has_value());
    CHECK_FALSE(AsStringView(boolValue).has_value());
    CHECK_FALSE(AsStringView(monoValue).has_value());
}

TEST_CASE("HoldsString covers both string alternatives", "[log_line][helpers]")
{
    CHECK(HoldsString(LogValue{std::string("a")}));
    CHECK(HoldsString(LogValue{std::string_view{"b"}}));

    CHECK_FALSE(HoldsString(LogValue{int64_t{1}}));
    CHECK_FALSE(HoldsString(LogValue{uint64_t{1}}));
    CHECK_FALSE(HoldsString(LogValue{1.5}));
    CHECK_FALSE(HoldsString(LogValue{true}));
    CHECK_FALSE(HoldsString(LogValue{std::monostate{}}));
}

TEST_CASE("ToOwnedLogValue copies string_view bytes and detaches from source storage", "[log_line][helpers]")
{
    // Allocate the source bytes on the heap so we can free them and prove
    // the owned LogValue does not retain a dangling view.
    auto source = std::make_unique<std::string>("ephemeral");
    const LogValue viewValue{std::string_view{*source}};

    LogValue owned = ToOwnedLogValue(viewValue);
    REQUIRE(std::holds_alternative<std::string>(owned));

    source.reset();

    // Reading bytes through the owned value must remain safe after the
    // backing storage is gone — that is the contract the helper is for.
    CHECK(std::get<std::string>(owned) == "ephemeral");

    // Non-string values pass through unchanged.
    const LogValue intValue{int64_t{-7}};
    LogValue intCopy = ToOwnedLogValue(intValue);
    REQUIRE(std::holds_alternative<int64_t>(intCopy));
    CHECK(std::get<int64_t>(intCopy) == -7);

    const LogValue monoCopy = ToOwnedLogValue(LogValue{std::monostate{}});
    CHECK(std::holds_alternative<std::monostate>(monoCopy));
}

TEST_CASE("LogValueEquivalent treats string and string_view byte-equal as equivalent", "[log_line][helpers]")
{
    const std::string owned = "hello";
    const std::string_view view = "hello";
    const std::string_view differentView = "world";

    CHECK(LogValueEquivalent(LogValue{owned}, LogValue{view}));
    CHECK(LogValueEquivalent(LogValue{view}, LogValue{owned}));
    CHECK(LogValueEquivalent(LogValue{owned}, LogValue{owned}));
    CHECK(LogValueEquivalent(LogValue{view}, LogValue{view}));

    CHECK_FALSE(LogValueEquivalent(LogValue{owned}, LogValue{differentView}));
    CHECK_FALSE(LogValueEquivalent(LogValue{owned}, LogValue{int64_t{0}}));
    CHECK_FALSE(LogValueEquivalent(LogValue{int64_t{1}}, LogValue{int64_t{2}}));
    CHECK(LogValueEquivalent(LogValue{int64_t{42}}, LogValue{int64_t{42}}));

    // Tag-mismatched non-string values must not be equivalent even when the
    // numeric payloads happen to coincide — the alternative is part of the
    // contract.
    CHECK_FALSE(LogValueEquivalent(LogValue{int64_t{1}}, LogValue{uint64_t{1}}));
    CHECK_FALSE(LogValueEquivalent(LogValue{1.0}, LogValue{int64_t{1}}));
}

TEST_CASE("AsEpochMicroseconds matches the TimeRangeRowPredicate slot acceptance set", "[log_line][helpers][time]")
{
    using namespace std::chrono;

    // `TimeStamp`: the dominant shape, returned as raw epoch micros.
    const TimeStamp ts{microseconds{1'700'000'123'456'789LL}};
    const auto fromTimestamp = AsEpochMicroseconds(LogValue{ts});
    REQUIRE(fromTimestamp.has_value());
    CHECK(*fromTimestamp == 1'700'000'123'456'789LL);

    // Promoted-int slots (epoch micros parsed as integers) pass
    // through; negatives included so pre-1970 timestamps survive.
    CHECK(AsEpochMicroseconds(LogValue{int64_t{42}}) == int64_t{42});
    CHECK(AsEpochMicroseconds(LogValue{int64_t{-7}}) == int64_t{-7});
    CHECK(AsEpochMicroseconds(LogValue{int64_t{0}}) == int64_t{0});
    CHECK(AsEpochMicroseconds(LogValue{std::numeric_limits<int64_t>::max()}) == std::numeric_limits<int64_t>::max());
    CHECK(AsEpochMicroseconds(LogValue{std::numeric_limits<int64_t>::min()}) == std::numeric_limits<int64_t>::min());

    // `uint64_t` casts down up to INT64_MAX; out-of-range returns
    // nullopt rather than wrapping to a negative int64.
    CHECK(AsEpochMicroseconds(LogValue{uint64_t{0}}) == int64_t{0});
    CHECK(AsEpochMicroseconds(LogValue{uint64_t{12345}}) == int64_t{12345});
    CHECK(
        AsEpochMicroseconds(LogValue{static_cast<uint64_t>(std::numeric_limits<int64_t>::max())}) ==
        std::numeric_limits<int64_t>::max()
    );
    CHECK_FALSE(
        AsEpochMicroseconds(LogValue{static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1}).has_value()
    );
    CHECK_FALSE(AsEpochMicroseconds(LogValue{std::numeric_limits<uint64_t>::max()}).has_value());

    // Non-time arms all return nullopt.
    CHECK_FALSE(AsEpochMicroseconds(LogValue{std::monostate{}}).has_value());
    CHECK_FALSE(AsEpochMicroseconds(LogValue{std::string_view{"2024-01-01"}}).has_value());
    CHECK_FALSE(AsEpochMicroseconds(LogValue{std::string{"2024-01-01"}}).has_value());
    CHECK_FALSE(AsEpochMicroseconds(LogValue{1.5}).has_value());
    CHECK_FALSE(AsEpochMicroseconds(LogValue{true}).has_value());
    CHECK_FALSE(AsEpochMicroseconds(LogValue{false}).has_value());
}

// Fast path (`GetValue(KeyId)` over the sorted-by-id flat vector) and slow
// path (`GetValue(string)` routing through the back-pointer) must observe
// the same value regardless of which alternative the parser chose.
TEST_CASE("LogLine GetValue and ValueByName agree under both string alternatives", "[log_line][helpers]")
{
    const TestLogFile testLogFile;
    auto source = testLogFile.CreateFileLineSource();

    KeyIndex keys;
    const KeyId viewKey = keys.GetOrInsert("view-key");
    const KeyId ownedKey = keys.GetOrInsert("owned-key");
    const KeyId intKey = keys.GetOrInsert("int-key");

    // Build the sorted-by-KeyId pair vector by hand so the fast path is
    // exercised directly. KeyId allocation is monotonic so the GetOrInsert
    // order above is also the sorted order here.
    std::vector<std::pair<KeyId, LogValue>> sorted;
    sorted.emplace_back(viewKey, LogValue{std::string_view{"view-bytes"}});
    sorted.emplace_back(ownedKey, LogValue{std::string{"owned-bytes"}});
    sorted.emplace_back(intKey, LogValue{int64_t{99}});

    const LogLine line(std::move(sorted), *source, 0);

    // Fast vs. slow path must round-trip the same alternative.
    const LogValue fastView = line.GetValue(viewKey);
    const LogValue slowView = test_common::ValueByName(line, keys, "view-key");
    REQUIRE(LogValueEquivalent(fastView, slowView));
    REQUIRE(AsStringView(fastView).has_value());
    CHECK(*AsStringView(fastView) == "view-bytes");

    const LogValue fastOwned = line.GetValue(ownedKey);
    const LogValue slowOwned = test_common::ValueByName(line, keys, "owned-key");
    REQUIRE(LogValueEquivalent(fastOwned, slowOwned));
    REQUIRE(AsStringView(fastOwned).has_value());
    CHECK(*AsStringView(fastOwned) == "owned-bytes");

    const LogValue fastInt = line.GetValue(intKey);
    const LogValue slowInt = test_common::ValueByName(line, keys, "int-key");
    REQUIRE(std::holds_alternative<int64_t>(fastInt));
    REQUIRE(std::holds_alternative<int64_t>(slowInt));
    CHECK(std::get<int64_t>(fastInt) == 99);
    CHECK(std::get<int64_t>(slowInt) == 99);
}

TEST_CASE("LogLine resolves DictRef slots through the source's EnumDictionaryRegistry", "[log_line][enum]")
{
    const TestLogFile testFile;
    auto source = testFile.CreateFileLineSource();

    EnumDictionaryRegistry registry;
    KeyIndex keys;
    const KeyId levelKey = keys.GetOrInsert("level");

    EnumDictionary &dict = registry.GetOrInsert(levelKey);
    const EnumValueId infoId = dict.Insert("info");
    const EnumValueId warnId = dict.Insert("warn");
    REQUIRE(infoId != INVALID_ENUM_VALUE_ID);
    REQUIRE(warnId != INVALID_ENUM_VALUE_ID);

    source->SetEnumDictionaries(&registry);

    const KeyId messageKey = keys.GetOrInsert("message");
    LogLine line = test_common::MakeLine({}, keys, *source, 1);
    line.SetValue(levelKey, LogValue{std::string("placeholder")});
    *line.FindCompactMutable(levelKey) = CompactLogValue::MakeDictRef(warnId).WithKey(levelKey);
    line.SetValue(messageKey, LogValue{std::string("hello")});

    REQUIRE(line.IsDictRef(levelKey));
    const LogValue resolved = line.GetValue(levelKey);
    const auto sv = AsStringView(resolved);
    REQUIRE(sv.has_value());
    CHECK(*sv == "warn");

    CHECK(std::get<std::string>(line.GetValue(messageKey)) == "hello");

    source->SetEnumDictionaries(nullptr);
    CHECK(std::holds_alternative<std::monostate>(line.GetValue(levelKey)));

    source->SetEnumDictionaries(&registry);
    // SetEnumDictionaries(&registry) above restores the dict; the slot is a known DictRef.
    CHECK(*AsStringView(line.GetValue(levelKey)) == "warn");
}

TEST_CASE("LogLine::IsDictRef discriminates DictRef slots from MmapSlice / OwnedString", "[log_line][enum][helpers]")
{
    const TestLogFile testFile;
    auto source = testFile.CreateFileLineSource();

    EnumDictionaryRegistry registry;
    KeyIndex keys;
    const KeyId enumKey = keys.GetOrInsert("level");
    const KeyId stringKey = keys.GetOrInsert("msg");
    const EnumValueId vid = registry.GetOrInsert(enumKey).Insert("info");

    source->SetEnumDictionaries(&registry);

    LogLine line = test_common::MakeLine({}, keys, *source, 1);
    line.SetValue(enumKey, LogValue{std::string("placeholder")});
    *line.FindCompactMutable(enumKey) = CompactLogValue::MakeDictRef(vid).WithKey(enumKey);
    line.SetValue(stringKey, LogValue{std::string("hello")});

    CHECK(line.IsDictRef(enumKey));
    CHECK_FALSE(line.IsDictRef(stringKey));
    CHECK(line.FindCompact(enumKey)->tag == CompactTag::DictRef);
    CHECK(line.FindCompact(stringKey)->tag == CompactTag::OwnedString);
}

TEST_CASE("Rows that borrow their fields from an arena behave like other rows", "[log_line][field_arena]")
{
    const TestLogFile testLogFile;
    testLogFile.Write("abcd\nefgh\n");
    auto source = testLogFile.CreateFileLineSource();

    KeyIndex keys;
    const KeyId first = keys.GetOrInsert(std::string_view("a"));
    const KeyId second = keys.GetOrInsert(std::string_view("b"));
    const KeyId third = keys.GetOrInsert(std::string_view("c"));

    FieldArena arena;
    std::vector<LogLine> lines;
    // Enough rows to span several chunks.
    constexpr int ROWS = 2000;
    for (int i = 0; i < ROWS; ++i)
    {
        std::vector<CompactLogValue> fields{
            CompactLogValue::MakeInt64(i).WithKey(first),
            CompactLogValue::MakeInt64(-i).WithKey(third),
        };
        lines.emplace_back(fields, arena, *source, 0);
    }

    SECTION("Reads see the stored values")
    {
        for (int i = 0; i < ROWS; ++i)
        {
            REQUIRE(lines[static_cast<size_t>(i)].ValueCount() == 2);
            CHECK(std::get<int64_t>(lines[static_cast<size_t>(i)].GetValue(first)) == i);
            CHECK(std::get<int64_t>(lines[static_cast<size_t>(i)].GetValue(third)) == -i);
            CHECK(loglib::internal::MemoryStats::RowOwnedBytes(lines[static_cast<size_t>(i)]) == 0);
        }
        CHECK(arena.MemoryBytes() > 0);
    }

    SECTION("Adding a field moves one row to the heap and leaves its neighbours alone")
    {
        lines[10].SetValue(second, LogValue{int64_t{99}});
        REQUIRE(lines[10].ValueCount() == 3);
        CHECK(std::get<int64_t>(lines[10].GetValue(second)) == 99);
        CHECK(std::get<int64_t>(lines[10].GetValue(first)) == 10);
        CHECK(std::get<int64_t>(lines[10].GetValue(third)) == -10);
        CHECK(loglib::internal::MemoryStats::RowOwnedBytes(lines[10]) > 0);
        CHECK(lines[11].ValueCount() == 2);
        CHECK(std::get<int64_t>(lines[11].GetValue(first)) == 11);
        CHECK(std::get<int64_t>(lines[9].GetValue(third)) == -9);

        // Updating a field in place keeps the row in the arena.
        lines[20].SetValue(first, LogValue{int64_t{7}});
        CHECK(loglib::internal::MemoryStats::RowOwnedBytes(lines[20]) == 0);
        CHECK(std::get<int64_t>(lines[20].GetValue(first)) == 7);
    }

    SECTION("A moved row keeps pointing at its fields")
    {
        LogLine moved = std::move(lines[5]);
        CHECK(std::get<int64_t>(moved.GetValue(first)) == 5);
        LogLine assigned(std::vector<CompactLogValue>{}, *source, 0);
        assigned = std::move(moved);
        CHECK(std::get<int64_t>(assigned.GetValue(third)) == -5);
    }
}

TEST_CASE("The test support helpers give back the map a line was made from", "[log_line][test_common]")
{
    const test_common::LogMap map{
        {"text", std::string("hello")},
        {"unsigned", static_cast<uint64_t>(42)},
        {"signed", static_cast<int64_t>(-12)},
        {"real", 2.5},
        {"flag", false},
        {"when", TimeStamp{std::chrono::microseconds{1'700'000'000'000'000}}},
    };

    const TestLogFile testLogFile;
    testLogFile.Write("abcd\n");
    auto source = testLogFile.CreateFileLineSource();

    KeyIndex keys;
    const LogLine line = test_common::MakeLine(map, keys, *source, 0);

    CHECK(test_common::ToMap(line, keys) == map);
    CHECK(test_common::KeyNames(line, keys).size() == map.size());
    CHECK(std::holds_alternative<std::monostate>(test_common::ValueByName(line, keys, "missing")));
}
