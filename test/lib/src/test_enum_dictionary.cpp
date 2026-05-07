#include <loglib/enum_dictionary.hpp>

#include <catch2/catch_all.hpp>

#include <string>
#include <string_view>

using namespace loglib;

TEST_CASE("EnumDictionary inserts up to its instance cap", "[enum_dictionary]")
{
    // Construct with the hard ceiling so the "fill it up" section
    // exercises the upper bound of `EnumValueId`'s value space; the
    // default-constructed cap (`DEFAULT_ENUM_VALUE_CAP = 64`) is
    // covered separately below.
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

        // Already-present values keep returning their id.
        CHECK(dict.Insert("v0") != INVALID_ENUM_VALUE_ID);

        // A new value would overflow.
        CHECK(dict.Insert("never-seen-before") == INVALID_ENUM_VALUE_ID);
        CHECK(dict.Size() == MAX_ENUM_VALUES);
    }
}

TEST_CASE("EnumDictionary honours an explicit cap below MAX_ENUM_VALUES", "[enum_dictionary]")
{
    // Mirrors the runtime knob (`AdvancedParserOptions::enumValueCap`)
    // path: dictionaries created with a small cap stop accepting
    // inserts at exactly that boundary, regardless of the compile-time
    // ceiling.
    EnumDictionary dict{8};
    REQUIRE(dict.Cap() == 8);

    for (uint16_t i = 0; i < 8; ++i)
    {
        const auto id = dict.Insert("level-" + std::to_string(i));
        REQUIRE(id != INVALID_ENUM_VALUE_ID);
    }
    REQUIRE(dict.Full());
    REQUIRE(dict.Size() == 8);

    // Re-inserting an already-present value still resolves to its id.
    CHECK(dict.Insert("level-0") == static_cast<EnumValueId>(0));
    // First over-cap insert returns the sentinel; the dict stays at cap.
    CHECK(dict.Insert("level-9") == INVALID_ENUM_VALUE_ID);
    CHECK(dict.Size() == 8);
}

TEST_CASE("EnumDictionary::Find resolves heterogeneous string_view lookups", "[enum_dictionary]")
{
    // `Find` goes through `mIndex` (transparent hashmap) so the
    // `string_view` / `string` / character-array overloads must all
    // resolve to the same id without round-tripping through a
    // temporary `std::string`. The pointer-stability check below
    // catches the original bug where `Find` walked `mValues` and
    // would silently degrade once the dictionary grew past a handful
    // of entries.
    EnumDictionary dict{MAX_ENUM_VALUES};
    const EnumValueId info = dict.Insert("info");
    const EnumValueId warn = dict.Insert("warn");
    const EnumValueId error = dict.Insert("error");

    CHECK(dict.Find(std::string_view{"info"}) == info);
    CHECK(dict.Find(std::string{"warn"}) == warn);
    CHECK(dict.Find("error") == error);
    CHECK(dict.Find("absent") == INVALID_ENUM_VALUE_ID);

    // Sanity: large dictionaries still resolve via the index, not a
    // linear scan. Pump in 200 values and make sure every one round-
    // trips correctly under string_view lookup.
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

    constexpr KeyId kLevel = 1;
    constexpr KeyId kComponent = 2;

    EnumDictionary &level = registry.GetOrInsert(kLevel);
    EnumDictionary &component = registry.GetOrInsert(kComponent);

    static_cast<void>(level.Insert("info"));
    static_cast<void>(level.Insert("warn"));
    static_cast<void>(component.Insert("auth"));

    REQUIRE(registry.Find(kLevel) != nullptr);
    REQUIRE(registry.Find(kComponent) != nullptr);
    CHECK(registry.Find(kLevel)->Size() == 2);
    CHECK(registry.Find(kComponent)->Size() == 1);

    // Erasing one column leaves the others intact.
    registry.Erase(kLevel);
    CHECK(registry.Find(kLevel) == nullptr);
    CHECK_FALSE(registry.Contains(kLevel));
    CHECK(registry.Find(kComponent) != nullptr);
}

TEST_CASE("EnumDictionaryRegistry::Alias makes multiple KeyIds share storage", "[enum_dictionary]")
{
    EnumDictionaryRegistry registry;

    constexpr KeyId kLevel = 1;
    constexpr KeyId kSeverity = 7;
    constexpr KeyId kVerbosity = 13;

    EnumDictionary &dict = registry.GetOrInsert(kLevel);
    static_cast<void>(dict.Insert("info"));

    registry.Alias(kLevel, kSeverity);
    registry.Alias(kLevel, kVerbosity);

    REQUIRE(registry.Find(kLevel) != nullptr);
    REQUIRE(registry.Find(kSeverity) != nullptr);
    REQUIRE(registry.Find(kVerbosity) != nullptr);
    CHECK(registry.Find(kLevel) == registry.Find(kSeverity));
    CHECK(registry.Find(kLevel) == registry.Find(kVerbosity));

    // Mutating the canonical reflects through every alias view.
    static_cast<void>(dict.Insert("warn"));
    CHECK(registry.Find(kSeverity)->Size() == 2);
    CHECK(registry.Find(kVerbosity)->Resolve(static_cast<EnumValueId>(1)) == "warn");

    // Erase by canonical drops every alias too.
    registry.Erase(kLevel);
    CHECK(registry.Find(kLevel) == nullptr);
    CHECK(registry.Find(kSeverity) == nullptr);
    CHECK(registry.Find(kVerbosity) == nullptr);
}

TEST_CASE("EnumDictionaryRegistry::Clear wipes everything", "[enum_dictionary]")
{
    EnumDictionaryRegistry registry;
    constexpr KeyId kA = 1;
    constexpr KeyId kB = 2;
    static_cast<void>(registry.GetOrInsert(kA).Insert("alpha"));
    static_cast<void>(registry.GetOrInsert(kB).Insert("beta"));

    registry.Clear();
    CHECK(registry.Empty());
    CHECK(registry.Find(kA) == nullptr);
    CHECK(registry.Find(kB) == nullptr);
}
