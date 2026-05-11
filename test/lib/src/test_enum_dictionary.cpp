#include <loglib/enum_dictionary.hpp>

#include <catch2/catch_all.hpp>

#include <string>
#include <string_view>

using namespace loglib;

TEST_CASE("EnumDictionary inserts up to its instance cap", "[enum_dictionary]")
{
    EnumDictionary dict{MAX_ENUM_VALUES};
    REQUIRE(dict.Empty());
    REQUIRE_FALSE(dict.Full());
    REQUIRE(dict.Cap() == MAX_ENUM_VALUES);

    SECTION("Inserting the same value twice returns the same id")
    {
        const EnumValueId first = dict.Insert("info");
        const EnumValueId second = dict.Insert("info");
        CHECK(first == second);
        CHECK(dict.Size() == 1);
    }

    SECTION("Insertion order maps to consecutive ids")
    {
        const EnumValueId info = dict.Insert("info");
        const EnumValueId warn = dict.Insert("warn");
        const EnumValueId error = dict.Insert("error");
        CHECK(static_cast<uint16_t>(info) == 0);
        CHECK(static_cast<uint16_t>(warn) == 1);
        CHECK(static_cast<uint16_t>(error) == 2);
        CHECK(dict.Resolve(info) == "info");
        CHECK(dict.Resolve(warn) == "warn");
        CHECK(dict.Resolve(error) == "error");
        CHECK(dict.Size() == 3);
    }

    SECTION("Inserting past the cap returns INVALID_ENUM_VALUE_ID")
    {
        for (uint16_t i = 0; i < MAX_ENUM_VALUES; ++i)
        {
            const auto id = dict.Insert("v" + std::to_string(i));
            REQUIRE(id != INVALID_ENUM_VALUE_ID);
        }
        REQUIRE(dict.Full());
        REQUIRE(dict.Size() == MAX_ENUM_VALUES);

        CHECK(dict.Insert("v0") != INVALID_ENUM_VALUE_ID);
        CHECK(dict.Insert("never-seen-before") == INVALID_ENUM_VALUE_ID);
        CHECK(dict.Size() == MAX_ENUM_VALUES);
    }
}

TEST_CASE("EnumDictionary honours an explicit cap below MAX_ENUM_VALUES", "[enum_dictionary]")
{
    EnumDictionary dict{8};
    REQUIRE(dict.Cap() == 8);

    for (uint16_t i = 0; i < 8; ++i)
    {
        const auto id = dict.Insert("level-" + std::to_string(i));
        REQUIRE(id != INVALID_ENUM_VALUE_ID);
    }
    REQUIRE(dict.Full());
    REQUIRE(dict.Size() == 8);

    CHECK(dict.Insert("level-0") == static_cast<EnumValueId>(0));
    CHECK(dict.Insert("level-9") == INVALID_ENUM_VALUE_ID);
    CHECK(dict.Size() == 8);
}

TEST_CASE("EnumDictionary::Find resolves heterogeneous string_view lookups", "[enum_dictionary]")
{
    EnumDictionary dict{MAX_ENUM_VALUES};
    const EnumValueId info = dict.Insert("info");
    const EnumValueId warn = dict.Insert("warn");
    const EnumValueId error = dict.Insert("error");

    CHECK(dict.Find(std::string_view{"info"}) == info);
    CHECK(dict.Find(std::string{"warn"}) == warn);
    CHECK(dict.Find("error") == error);
    CHECK(dict.Find("absent") == INVALID_ENUM_VALUE_ID);

    for (int i = 0; i < 200; ++i)
    {
        const std::string value = "bulk-" + std::to_string(i);
        const EnumValueId id = dict.Insert(value);
        REQUIRE(id != INVALID_ENUM_VALUE_ID);
        CHECK(dict.Find(std::string_view{value}) == id);
    }
}

TEST_CASE("EnumDictionary::Find returns sentinel when bytes are not interned", "[enum_dictionary]")
{
    EnumDictionary dict;
    CHECK(dict.Find("absent") == INVALID_ENUM_VALUE_ID);
    static_cast<void>(dict.Insert("present"));
    CHECK(dict.Find("absent") == INVALID_ENUM_VALUE_ID);
    CHECK(dict.Find("present") != INVALID_ENUM_VALUE_ID);
}

TEST_CASE("EnumDictionary::Resolve returns empty view for out-of-range ids", "[enum_dictionary]")
{
    EnumDictionary dict;
    CHECK(dict.Resolve(INVALID_ENUM_VALUE_ID).empty());
    static_cast<void>(dict.Insert("alpha"));
    CHECK(dict.Resolve(static_cast<EnumValueId>(5)).empty());
}

TEST_CASE("EnumDictionary::Clear drops every entry", "[enum_dictionary]")
{
    EnumDictionary dict;
    static_cast<void>(dict.Insert("info"));
    static_cast<void>(dict.Insert("warn"));
    REQUIRE(dict.Size() == 2);

    dict.Clear();
    CHECK(dict.Empty());
    CHECK(dict.Size() == 0);
    CHECK(dict.Find("info") == INVALID_ENUM_VALUE_ID);
}

TEST_CASE("EnumDictionaryRegistry isolates per-column dictionaries", "[enum_dictionary]")
{
    EnumDictionaryRegistry registry;

    constexpr KeyId LEVEL = 1;
    constexpr KeyId COMPONENT = 2;

    EnumDictionary &level = registry.GetOrInsert(LEVEL);
    EnumDictionary &component = registry.GetOrInsert(COMPONENT);

    static_cast<void>(level.Insert("info"));
    static_cast<void>(level.Insert("warn"));
    static_cast<void>(component.Insert("auth"));

    REQUIRE(registry.Find(LEVEL) != nullptr);
    REQUIRE(registry.Find(COMPONENT) != nullptr);
    CHECK(registry.Find(LEVEL)->Size() == 2);
    CHECK(registry.Find(COMPONENT)->Size() == 1);

    registry.Erase(LEVEL);
    CHECK(registry.Find(LEVEL) == nullptr);
    CHECK_FALSE(registry.Contains(LEVEL));
    CHECK(registry.Find(COMPONENT) != nullptr);
}

TEST_CASE("EnumDictionaryRegistry::Alias makes multiple KeyIds share storage", "[enum_dictionary]")
{
    EnumDictionaryRegistry registry;

    constexpr KeyId LEVEL = 1;
    constexpr KeyId SEVERITY = 7;
    constexpr KeyId VERBOSITY = 13;

    EnumDictionary &dict = registry.GetOrInsert(LEVEL);
    static_cast<void>(dict.Insert("info"));

    REQUIRE(registry.Alias(LEVEL, SEVERITY));
    REQUIRE(registry.Alias(LEVEL, VERBOSITY));
    REQUIRE(registry.Alias(LEVEL, SEVERITY));
    REQUIRE(registry.Alias(LEVEL, LEVEL));
    constexpr KeyId MISSING_CANONICAL = 99;
    constexpr KeyId MISSING_ALIAS = 100;
    CHECK_FALSE(registry.Alias(MISSING_CANONICAL, MISSING_ALIAS));

    REQUIRE(registry.Find(LEVEL) != nullptr);
    REQUIRE(registry.Find(SEVERITY) != nullptr);
    REQUIRE(registry.Find(VERBOSITY) != nullptr);
    CHECK(registry.Find(LEVEL) == registry.Find(SEVERITY));
    CHECK(registry.Find(LEVEL) == registry.Find(VERBOSITY));

    static_cast<void>(dict.Insert("warn"));
    CHECK(registry.Find(SEVERITY)->Size() == 2);
    CHECK(registry.Find(VERBOSITY)->Resolve(static_cast<EnumValueId>(1)) == "warn");

    registry.Erase(LEVEL);
    CHECK(registry.Find(LEVEL) == nullptr);
    CHECK(registry.Find(SEVERITY) == nullptr);
    CHECK(registry.Find(VERBOSITY) == nullptr);
}

TEST_CASE("EnumDictionaryRegistry::GetOrInsert honours existing alias mapping", "[enum_dictionary]")
{
    // Regression: `GetOrInsert(alias_key)` used to clobber an existing
    // alias-to-canonical mapping with a fresh dictionary.
    EnumDictionaryRegistry registry;

    constexpr KeyId LEVEL = 1;
    constexpr KeyId SEVERITY = 2;

    EnumDictionary &canonical = registry.GetOrInsert(LEVEL);
    static_cast<void>(canonical.Insert("info"));
    REQUIRE(registry.Alias(LEVEL, SEVERITY));

    EnumDictionary &resolved = registry.GetOrInsert(SEVERITY);
    CHECK(&resolved == &canonical);
    CHECK(resolved.Size() == 1);
    CHECK(resolved.Find("info") != INVALID_ENUM_VALUE_ID);

    registry.Erase(LEVEL);
    CHECK(registry.Find(LEVEL) == nullptr);
    CHECK(registry.Find(SEVERITY) == nullptr);
    CHECK(registry.Empty());
}

TEST_CASE("EnumDictionaryRegistry::Clear wipes everything", "[enum_dictionary]")
{
    EnumDictionaryRegistry registry;
    constexpr KeyId A = 1;
    constexpr KeyId B = 2;
    static_cast<void>(registry.GetOrInsert(A).Insert("alpha"));
    static_cast<void>(registry.GetOrInsert(B).Insert("beta"));

    registry.Clear();
    CHECK(registry.Empty());
    CHECK(registry.Find(A) == nullptr);
    CHECK(registry.Find(B) == nullptr);
}
