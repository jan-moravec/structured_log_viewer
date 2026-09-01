#include <loglib/internal/column_key_id_cache.hpp>
#include <loglib/key_index.hpp>
#include <loglib/log_configuration.hpp>

#include <catch2/catch_all.hpp>

#include <string>
#include <vector>

using namespace loglib;

TEST_CASE("RefreshColumnKeyIds records INVALID_KEY_ID for interned-missing keys", "[column_key_id_cache]")
{
    LogConfiguration configuration;
    configuration.columns.push_back(
        Column{
            .header = "msg",
            .keys = {"msg", "message"},
            .printFormat = "{}",
            .type = ColumnType::String,
        }
    );
    configuration.columns.push_back(
        Column{
            .header = "level",
            .keys = {"level"},
            .printFormat = "{}",
            .type = ColumnType::Level,
        }
    );

    KeyIndex keys;
    const KeyId msgId = keys.GetOrInsert("msg");

    std::vector<std::vector<KeyId>> cache;
    internal::RefreshColumnKeyIds(cache, configuration, keys);

    REQUIRE(cache.size() == 2);
    REQUIRE(cache[0].size() == 2);
    CHECK(cache[0][0] == msgId);
    CHECK(cache[0][1] == INVALID_KEY_ID);
    REQUIRE(cache[1].size() == 1);
    CHECK(cache[1][0] == INVALID_KEY_ID);
}

TEST_CASE(
    "RefreshColumnKeyIdsForKeys resizes after a shrinking schema and rewrites affected columns", "[column_key_id_cache]"
)
{
    LogConfiguration configuration;
    configuration.columns.push_back(Column{.header = "a", .keys = {"a"}, .printFormat = "{}", .type = ColumnType::Any});
    configuration.columns.push_back(Column{.header = "b", .keys = {"b"}, .printFormat = "{}", .type = ColumnType::Any});

    KeyIndex keys;
    const KeyId aId = keys.GetOrInsert("a");
    const KeyId bId = keys.GetOrInsert("b");

    std::vector<std::vector<KeyId>> cache{{aId}, {INVALID_KEY_ID}, {INVALID_KEY_ID}};
    internal::RefreshColumnKeyIdsForKeys(cache, configuration, keys, {"b"});
    REQUIRE(cache.size() == 2);
    CHECK(cache[0] == std::vector<KeyId>{aId});
    CHECK(cache[1] == std::vector<KeyId>{bId});
}

TEST_CASE("MoveColumnKeyIds rotates cache entries with the column order", "[column_key_id_cache]")
{
    std::vector<std::vector<KeyId>> cache{{KeyId{1}}, {KeyId{2}}, {KeyId{3}}};
    internal::MoveColumnKeyIds(cache, 2, 0);
    REQUIRE(cache.size() == 3);
    CHECK(cache[0] == std::vector<KeyId>{KeyId{3}});
    CHECK(cache[1] == std::vector<KeyId>{KeyId{1}});
    CHECK(cache[2] == std::vector<KeyId>{KeyId{2}});

    internal::MoveColumnKeyIds(cache, 0, 0);
    CHECK(cache[0] == std::vector<KeyId>{KeyId{3}});

    internal::MoveColumnKeyIds(cache, 9, 0);
    CHECK(cache.size() == 3);
}
