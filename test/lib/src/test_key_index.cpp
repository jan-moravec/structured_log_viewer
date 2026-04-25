#include <loglib/key_index.hpp>

#include <catch2/catch_all.hpp>
#include <oneapi/tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <set>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace loglib;

TEST_CASE("KeyIndex assigns dense, monotonically increasing ids starting at 0", "[key_index]")
{
    KeyIndex index;
    REQUIRE(index.Size() == 0);

    const KeyId a = index.GetOrInsert("alpha");
    const KeyId b = index.GetOrInsert("beta");
    const KeyId c = index.GetOrInsert("gamma");

    CHECK(a == 0);
    CHECK(b == 1);
    CHECK(c == 2);
    CHECK(index.Size() == 3);
}

TEST_CASE("KeyIndex GetOrInsert returns the existing id for repeat inserts", "[key_index]")
{
    KeyIndex index;
    const KeyId first = index.GetOrInsert("repeat");
    const KeyId second = index.GetOrInsert("repeat");
    const KeyId third = index.GetOrInsert(std::string("repeat")); // Heterogeneous overload via string_view ctor

    CHECK(first == second);
    CHECK(first == third);
    CHECK(index.Size() == 1);
}

TEST_CASE("KeyIndex Find returns kInvalidKeyId for absent keys", "[key_index]")
{
    KeyIndex index;
    CHECK(index.Find("missing") == kInvalidKeyId);

    const KeyId id = index.GetOrInsert("present");
    CHECK(index.Find("present") == id);
    CHECK(index.Find("missing") == kInvalidKeyId);
}

TEST_CASE("KeyIndex KeyOf round-trips inserted keys", "[key_index]")
{
    KeyIndex index;
    const std::vector<std::string> keys = {"first", "second", "third", "fourth"};

    std::vector<KeyId> ids;
    ids.reserve(keys.size());
    for (const auto &k : keys)
    {
        ids.push_back(index.GetOrInsert(k));
    }

    REQUIRE(ids.size() == keys.size());
    for (size_t i = 0; i < ids.size(); ++i)
    {
        INFO("i = " << i);
        CHECK(index.KeyOf(ids[i]) == keys[i]);
    }
}

TEST_CASE("KeyIndex KeyOf returns string_views that are stable across further inserts", "[key_index]")
{
    KeyIndex index;
    const KeyId firstId = index.GetOrInsert("stable");
    const std::string_view firstView = index.KeyOf(firstId);
    const char *firstData = firstView.data();
    const size_t firstSize = firstView.size();

    // Force the underlying storage to grow well past the first entry.
    for (int i = 0; i < 1000; ++i)
    {
        index.GetOrInsert("k" + std::to_string(i));
    }

    const std::string_view firstViewAgain = index.KeyOf(firstId);
    CHECK(firstViewAgain.data() == firstData); // pointer stability across inserts
    CHECK(firstViewAgain.size() == firstSize);
    CHECK(firstViewAgain == "stable");
}

TEST_CASE("KeyIndex SortedKeys returns a sorted, deduplicated snapshot", "[key_index]")
{
    KeyIndex index;
    index.GetOrInsert("delta");
    index.GetOrInsert("alpha");
    index.GetOrInsert("charlie");
    index.GetOrInsert("alpha");   // duplicate insert
    index.GetOrInsert("bravo");

    const std::vector<std::string> sorted = index.SortedKeys();
    REQUIRE(sorted.size() == 4);
    CHECK(sorted == std::vector<std::string>{"alpha", "bravo", "charlie", "delta"});
}

TEST_CASE("KeyIndex Size matches insertion count", "[key_index]")
{
    KeyIndex index;
    for (int i = 0; i < 10; ++i)
    {
        index.GetOrInsert("k" + std::to_string(i));
        CHECK(index.Size() == static_cast<size_t>(i + 1));
    }

    // Re-inserts do not bump the count.
    index.GetOrInsert("k0");
    CHECK(index.Size() == 10);
}

TEST_CASE("KeyIndex GetOrInsert is safe under concurrent contention with overlapping key sets", "[key_index]")
{
    constexpr int kKeyCount = 100;
    constexpr int kThreadCount = 8;

    // Pre-build the key strings so the concurrent loop does only inserts (no string formatting
    // contention via the global allocator).
    std::vector<std::string> keys;
    keys.reserve(kKeyCount);
    for (int i = 0; i < kKeyCount; ++i)
    {
        keys.push_back("k" + std::to_string(i));
    }

    KeyIndex index;

    // Each iteration inserts every key, so every worker race-inserts overlapping ids. The
    // postcondition is that exactly kKeyCount distinct ids exist and every key maps to exactly
    // one of them.
    oneapi::tbb::parallel_for(0, kThreadCount, [&](int /*thread*/) {
        for (const auto &k : keys)
        {
            // Throwaway return — we only care that all calls converge on the same
            // canonical id afterwards.
            (void)index.GetOrInsert(k);
        }
    });

    REQUIRE(index.Size() == static_cast<size_t>(kKeyCount));

    // Every key resolves to exactly one id.
    std::set<KeyId> seenIds;
    for (const auto &k : keys)
    {
        const KeyId id = index.Find(k);
        REQUIRE(id != kInvalidKeyId);
        const auto [it, inserted] = seenIds.insert(id);
        INFO("Duplicate id " << id << " observed for key " << k);
        REQUIRE(inserted);
    }
    CHECK(seenIds.size() == static_cast<size_t>(kKeyCount));

    // Ids are dense in [0, kKeyCount).
    CHECK(*seenIds.begin() == 0);
    CHECK(*seenIds.rbegin() == static_cast<KeyId>(kKeyCount - 1));

    // KeyOf returns stable string_views matching the original key bytes.
    for (const auto &k : keys)
    {
        const KeyId id = index.Find(k);
        REQUIRE(id != kInvalidKeyId);
        CHECK(index.KeyOf(id) == k);
    }
}

TEST_CASE("KeyIndex move construction preserves the dictionary", "[key_index]")
{
    KeyIndex source;
    const KeyId a = source.GetOrInsert("alpha");
    const KeyId b = source.GetOrInsert("beta");

    KeyIndex moved(std::move(source));
    CHECK(moved.Size() == 2);
    CHECK(moved.Find("alpha") == a);
    CHECK(moved.Find("beta") == b);
    CHECK(moved.KeyOf(a) == "alpha");
    CHECK(moved.KeyOf(b) == "beta");
}
