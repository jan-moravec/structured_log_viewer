#include <loglib/key_index.hpp>

#include <catch2/catch_all.hpp>
#include <oneapi/tbb/parallel_for.h>

#include <algorithm>
#include <atomic>
#include <random>
#include <set>
#include <string>
#include <string_view>
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
    index.GetOrInsert("alpha"); // duplicate insert
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

// Heterogeneous-lookup race stress test. Eight workers race
// `GetOrInsert`/`Find` against a small (200-key) overlapping pool.
// Postcondition: exactly 200 distinct ids, every Find matches its
// GetOrInsert, KeyOf round-trips correctly, and ids are dense in [0, 200).
TEST_CASE("KeyIndex heterogeneous fast path is safe under concurrent insert+find storm", "[key_index][stress]")
{
    constexpr int kKeyCount = 200;
    constexpr int kThreadCount = 8;
    constexpr int kIterationsPerThread = 100'000;

    std::vector<std::string> keys;
    keys.reserve(kKeyCount);
    for (int i = 0; i < kKeyCount; ++i)
    {
        keys.push_back("stress_key_" + std::to_string(i));
    }

    KeyIndex index;

    oneapi::tbb::parallel_for(0, kThreadCount, [&](int thread) {
        // Per-thread RNG so the iteration order interleaves but is reproducible.
        std::mt19937 rng(static_cast<unsigned>(thread) * 37u + 1u);
        std::uniform_int_distribution<int> pickKey(0, kKeyCount - 1);
        for (int i = 0; i < kIterationsPerThread; ++i)
        {
            const std::string &k = keys[static_cast<size_t>(pickKey(rng))];
            // Mix of GetOrInsert + Find so both code paths race against
            // each other on the same shard. Throwaway results — the
            // postcondition is checked once after the parallel_for.
            (void)index.GetOrInsert(k);
            (void)index.Find(k);
        }
    });

    REQUIRE(index.Size() == static_cast<size_t>(kKeyCount));

    std::set<KeyId> seenIds;
    for (const auto &k : keys)
    {
        const KeyId byFind = index.Find(k);
        REQUIRE(byFind != kInvalidKeyId);
        const KeyId byGetOrInsert = index.GetOrInsert(k);
        CHECK(byFind == byGetOrInsert);
        const auto [it, inserted] = seenIds.insert(byFind);
        INFO("Duplicate id " << byFind << " observed for key " << k);
        REQUIRE(inserted);
        CHECK(index.KeyOf(byFind) == k);
    }
    CHECK(*seenIds.begin() == 0);
    CHECK(*seenIds.rbegin() == static_cast<KeyId>(kKeyCount - 1));
    CHECK(index.Size() == static_cast<size_t>(kKeyCount));
}

// Heterogeneous-lookup unit test. Exercises the no-alloc fast path: a
// `string_view` query (built from a stack `char` buffer) must succeed
// without the caller materialising a `std::string`. The instrumentation
// counters are reset at the top and read at the end.
TEST_CASE(
    "KeyIndex heterogeneous lookup never requires the caller to materialise a std::string", "[key_index][heterogeneous]"
)
{
    KeyIndex index;
    const KeyId alphaId = index.GetOrInsert("alpha");
    const KeyId betaId = index.GetOrInsert("beta");

    // Counters are process-global; reset right before the measured calls so
    // any other test interleaving does not leak count noise into us.
    KeyIndex::ResetInstrumentationCounters();

    // Build the query as a string_view over a stack-allocated char buffer so
    // there is no chance of an implicit std::string ever being constructed
    // along the call path.
    constexpr char alphaBuf[] = "alpha";
    constexpr char betaBuf[] = "beta";
    constexpr char absentBuf[] = "missing";
    const std::string_view alphaView(alphaBuf, sizeof(alphaBuf) - 1);
    const std::string_view betaView(betaBuf, sizeof(betaBuf) - 1);
    const std::string_view absentView(absentBuf, sizeof(absentBuf) - 1);

    CHECK(index.Find(alphaView) == alphaId);
    CHECK(index.Find(betaView) == betaId);
    CHECK(index.Find(absentView) == kInvalidKeyId);

    // Repeat-`GetOrInsert` for an existing key must take the fast path
    // (heterogeneous `find` on the per-shard map) and not allocate a new id.
    CHECK(index.GetOrInsert(alphaView) == alphaId);
    CHECK(index.GetOrInsert(betaView) == betaId);
    CHECK(index.Size() == 2);

    // Counter assertions: 3 Find calls, 2 GetOrInsert calls. If the
    // implementation regressed and a heterogeneous find path now constructs
    // a std::string, this test still passes — the counter only proves the
    // public API was exercised the expected number of times. The non-
    // allocation contract itself is enforced by the `[allocations]`
    // benchmark's `string_view` fast-path fraction staying green.
    CHECK(KeyIndex::LoadFindCount() == 3);
    CHECK(KeyIndex::LoadGetOrInsertCount() == 2);
}
