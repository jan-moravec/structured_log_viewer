#include "common.hpp"

#include <loglib/key_index.hpp>
#include <loglib/log_file.hpp>
#include <loglib/log_line.hpp>

#include <catch2/catch_all.hpp>

using namespace loglib;

TEST_CASE("Construct LogLine with valid values and file reference", "[log_line]")
{
    LogMap map{
        {"key1", std::string("value1")},
        {"key2", uint64_t(42)},
        {"key3", int64_t(-12)},
        {"key4", 3.14},
        {"key5", true},
        {"key6", std::monostate()}
    };

    TestLogFile testLogFile;
    testLogFile.Write("abcd\nefgh\n");
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    LogFileReference fileReference(*logFile, 1);

    KeyIndex keys;
    LogLine line(map, keys, fileReference);

    REQUIRE(line.Values().size() == map.size());
    CHECK(std::get<std::string>(line.GetValue("key1")) == "value1");
    CHECK(std::get<uint64_t>(line.GetValue("key2")) == 42);
    CHECK(std::get<int64_t>(line.GetValue("key3")) == -12);
    CHECK(std::get<double>(line.GetValue("key4")) == Catch::Approx(3.14));
    CHECK(std::get<bool>(line.GetValue("key5")) == true);
    CHECK(std::holds_alternative<std::monostate>(line.GetValue("key6")));

    CHECK(line.FileReference().GetPath() == "test_file.json");
    CHECK(line.FileReference().GetLineNumber() == 1);
    CHECK(line.FileReference().GetLine() == "efgh");

    auto resultKeys = line.GetKeys();
    REQUIRE(resultKeys.size() == map.size());
    CHECK(std::find(resultKeys.begin(), resultKeys.end(), "key1") != resultKeys.end());
    CHECK(std::find(resultKeys.begin(), resultKeys.end(), "key2") != resultKeys.end());
    CHECK(std::find(resultKeys.begin(), resultKeys.end(), "key3") != resultKeys.end());
    CHECK(std::find(resultKeys.begin(), resultKeys.end(), "key4") != resultKeys.end());
    CHECK(std::find(resultKeys.begin(), resultKeys.end(), "key5") != resultKeys.end());
    CHECK(std::find(resultKeys.begin(), resultKeys.end(), "key6") != resultKeys.end());

    const LogMap resultMap = line.Values();

    REQUIRE(resultMap.size() == map.size());

    for (const auto &[key, expectedValue] : map)
    {
        REQUIRE(resultMap.count(key) == 1);

        std::visit(
            [&](const auto &expected) {
                using T = std::decay_t<decltype(expected)>;
                const auto &actual = std::get<T>(resultMap.at(key));

                if constexpr (std::is_same_v<T, double>)
                    CHECK(actual == Catch::Approx(expected));
                else
                    CHECK(actual == expected);
            },
            expectedValue
        );
    }
}

TEST_CASE("LogLine GetKeys returns empty vector for empty LogLine", "[log_line]")
{
    LogMap emptyMap;

    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    LogFileReference fileReference(*logFile, 0);

    KeyIndex keys;
    LogLine emptyLine(emptyMap, keys, fileReference);

    auto resultKeys = emptyLine.GetKeys();
    CHECK(resultKeys.empty());
    CHECK(emptyLine.Values().empty());
}

TEST_CASE("LogLine returns monostate for empty and non-existent key", "[log_line]")
{
    LogMap map{{"key1", std::string("value1")}, {"key2", uint64_t(42)}};

    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    LogFileReference fileReference(*logFile, 0);

    KeyIndex keys;
    LogLine line(map, keys, fileReference);

    CHECK(std::holds_alternative<std::monostate>(line.GetValue("")));
    CHECK(std::holds_alternative<std::monostate>(line.GetValue("non_existent_key")));

    CHECK(std::get<std::string>(line.GetValue("key1")) == "value1");
    CHECK(std::get<uint64_t>(line.GetValue("key2")) == 42);
}

TEST_CASE("Set and update values", "[log_line]")
{
    LogMap map{{"existingKey", std::string("initialValue")}};

    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    LogFileReference fileReference(*logFile, 0);

    KeyIndex keys;
    LogLine line(map, keys, fileReference);

    CHECK(std::get<std::string>(line.GetValue("existingKey")) == "initialValue");
    CHECK(line.Values().size() == 1);

    const std::string updatedValue = "updatedValue";
    line.SetValue("existingKey", updatedValue);

    CHECK(std::get<std::string>(line.GetValue("existingKey")) == updatedValue);
    CHECK(line.Values().size() == 1);

    auto resultKeys = line.GetKeys();
    REQUIRE(resultKeys.size() == 1);
    CHECK(resultKeys[0] == "existingKey");

    // SetValue(string) requires the key to be registered. Pre-insert it via the KeyIndex
    // because LogLine intentionally refuses to silently grow the dictionary on the slow path.
    const std::string newKey = "newKey";
    const std::string newValue = "newValue";
    keys.GetOrInsert(newKey);
    line.SetValue(newKey, newValue);

    REQUIRE(line.Values().size() == 2);
    CHECK(std::get<std::string>(line.GetValue(newKey)) == newValue);

    resultKeys = line.GetKeys();
    REQUIRE(resultKeys.size() == 2);
    CHECK(std::find(resultKeys.begin(), resultKeys.end(), newKey) != resultKeys.end());
}

// PRD req. 4.1.6 / 4.1.16 — the AsStringView / HoldsString / ToOwnedLogValue
// / LogValueEquivalent helpers are the public seam every consumer outside the
// parser uses to read string-typed values without caring whether the parser
// happened to land on the view or owned alternative. Pin the contract so the
// downstream conditional in (e.g.) LogModel::data and JsonParser::ToString
// keeps working as the parser shifts more values onto the fast path.
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
    LogValue viewValue{std::string_view{*source}};

    LogValue owned = ToOwnedLogValue(viewValue);
    REQUIRE(std::holds_alternative<std::string>(owned));

    source.reset();

    // Reading bytes through the owned value must remain safe after the
    // backing storage is gone — that is the contract the helper is for.
    CHECK(std::get<std::string>(owned) == "ephemeral");

    // Non-string values pass through unchanged.
    LogValue intValue{int64_t{-7}};
    LogValue intCopy = ToOwnedLogValue(intValue);
    REQUIRE(std::holds_alternative<int64_t>(intCopy));
    CHECK(std::get<int64_t>(intCopy) == -7);

    LogValue monoCopy = ToOwnedLogValue(LogValue{std::monostate{}});
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

// PRD req. 4.1.16 — fast path (GetValue(KeyId) over a sorted-by-id flat
// vector) and slow path (GetValue(string) routing through the back-pointer)
// must observe the same value regardless of the alternative the parser chose.
TEST_CASE("LogLine fast and slow GetValue accessors agree under both string alternatives", "[log_line][helpers]")
{
    TestLogFile testLogFile;
    std::unique_ptr<LogFile> logFile = testLogFile.CreateLogFile();
    LogFileReference fileReference(*logFile, 0);

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

    LogLine line(std::move(sorted), keys, fileReference);

    // Fast vs. slow path must round-trip the same alternative.
    const LogValue fastView = line.GetValue(viewKey);
    const LogValue slowView = line.GetValue(std::string("view-key"));
    REQUIRE(LogValueEquivalent(fastView, slowView));
    REQUIRE(AsStringView(fastView).has_value());
    CHECK(*AsStringView(fastView) == "view-bytes");

    const LogValue fastOwned = line.GetValue(ownedKey);
    const LogValue slowOwned = line.GetValue(std::string("owned-key"));
    REQUIRE(LogValueEquivalent(fastOwned, slowOwned));
    REQUIRE(AsStringView(fastOwned).has_value());
    CHECK(*AsStringView(fastOwned) == "owned-bytes");

    const LogValue fastInt = line.GetValue(intKey);
    const LogValue slowInt = line.GetValue(std::string("int-key"));
    REQUIRE(std::holds_alternative<int64_t>(fastInt));
    REQUIRE(std::holds_alternative<int64_t>(slowInt));
    CHECK(std::get<int64_t>(fastInt) == 99);
    CHECK(std::get<int64_t>(slowInt) == 99);
}
