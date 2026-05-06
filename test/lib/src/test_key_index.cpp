#include <loglib/key_index.hpp>

#include <catch2/catch_all.hpp>
#include <oneapi/tbb/parallel_for.h>

#include <atomic>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <thread>
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

TEST_CASE("KeyIndex Find returns INVALID_KEY_ID for absent keys", "[key_index]")
{
    KeyIndex index;
    CHECK(index.Find("missing") == INVALID_KEY_ID);

    const KeyId id = index.GetOrInsert("present");
    CHECK(index.Find("present") == id);
    CHECK(index.Find("missing") == INVALID_KEY_ID);
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
        static_cast<void>(index.GetOrInsert("k" + std::to_string(i)));
    }

    const std::string_view firstViewAgain = index.KeyOf(firstId);
    CHECK(firstViewAgain.data() == firstData); // pointer stability across inserts
    CHECK(firstViewAgain.size() == firstSize);
    CHECK(firstViewAgain == "stable");
}

TEST_CASE("KeyIndex SortedKeys returns a sorted, deduplicated snapshot", "[key_index]")
{
    KeyIndex index;
    static_cast<void>(index.GetOrInsert("delta"));
    static_cast<void>(index.GetOrInsert("alpha"));
    static_cast<void>(index.GetOrInsert("charlie"));
    static_cast<void>(index.GetOrInsert("alpha")); // duplicate insert
    static_cast<void>(index.GetOrInsert("bravo"));

    const std::vector<std::string> sorted = index.SortedKeys();
    REQUIRE(sorted.size() == 4);
    CHECK(sorted == std::vector<std::string>{"alpha", "bravo", "charlie", "delta"});
}

TEST_CASE("KeyIndex Size matches insertion count", "[key_index]")
{
    KeyIndex index;
    for (int i = 0; i < 10; ++i)
    {
        static_cast<void>(index.GetOrInsert("k" + std::to_string(i)));
        CHECK(index.Size() == static_cast<size_t>(i + 1));
    }

    // Re-inserts do not bump the count.
    static_cast<void>(index.GetOrInsert("k0"));
    CHECK(index.Size() == 10);
}

TEST_CASE("KeyIndex GetOrInsert is safe under concurrent contention with overlapping key sets", "[key_index]")
{
    constexpr int KEY_COUNT = 100;
    constexpr int THREAD_COUNT = 8;

    // Pre-build the key strings so the concurrent loop does only inserts (no string formatting
    // contention via the global allocator).
    std::vector<std::string> keys;
    keys.reserve(KEY_COUNT);
    for (int i = 0; i < KEY_COUNT; ++i)
    {
        keys.push_back("k" + std::to_string(i));
    }

    KeyIndex index;

    // Each iteration inserts every key, so every worker race-inserts overlapping ids. The
    // postcondition is that exactly KEY_COUNT distinct ids exist and every key maps to exactly
    // one of them.
    oneapi::tbb::parallel_for(0, THREAD_COUNT, [&](int /*thread*/) {
        for (const auto &k : keys)
        {
            // Throwaway return — we only care that all calls converge on the same
            // canonical id afterwards.
            (void)index.GetOrInsert(k);
        }
    });

    REQUIRE(index.Size() == static_cast<size_t>(KEY_COUNT));

    // Every key resolves to exactly one id.
    std::set<KeyId> seenIds;
    for (const auto &k : keys)
    {
        const KeyId id = index.Find(k);
        REQUIRE(id != INVALID_KEY_ID);
        const auto [it, inserted] = seenIds.insert(id);
        INFO("Duplicate id " << id << " observed for key " << k);
        REQUIRE(inserted);
    }
    CHECK(seenIds.size() == static_cast<size_t>(KEY_COUNT));

    // Ids are dense in [0, KEY_COUNT).
    CHECK(*seenIds.begin() == 0);
    CHECK(*seenIds.rbegin() == static_cast<KeyId>(KEY_COUNT - 1));

    // KeyOf returns stable string_views matching the original key bytes.
    for (const auto &k : keys)
    {
        const KeyId id = index.Find(k);
        REQUIRE(id != INVALID_KEY_ID);
        CHECK(index.KeyOf(id) == k);
    }
}

TEST_CASE("KeyIndex move construction preserves the dictionary", "[key_index]")
{
    KeyIndex source;
    const KeyId a = source.GetOrInsert("alpha");
    const KeyId b = source.GetOrInsert("beta");

    const KeyIndex moved(std::move(source));
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
    constexpr int KEY_COUNT = 200;
    constexpr int THREAD_COUNT = 8;
    constexpr int ITERATIONS_PER_THREAD = 100'000;

    std::vector<std::string> keys;
    keys.reserve(KEY_COUNT);
    for (int i = 0; i < KEY_COUNT; ++i)
    {
        keys.push_back("stress_key_" + std::to_string(i));
    }

    KeyIndex index;

    oneapi::tbb::parallel_for(0, THREAD_COUNT, [&](int thread) {
        // Per-thread RNG so the iteration order interleaves but is reproducible.
        std::mt19937 rng((static_cast<unsigned>(thread) * 37u) + 1u);
        std::uniform_int_distribution<int> pickKey(0, KEY_COUNT - 1);
        for (int i = 0; i < ITERATIONS_PER_THREAD; ++i)
        {
            const std::string &k = keys[static_cast<size_t>(pickKey(rng))];
            // Mix of GetOrInsert + Find so both code paths race against
            // each other on the same shard. Throwaway results — the
            // postcondition is checked once after the parallel_for.
            (void)index.GetOrInsert(k);
            (void)index.Find(k);
        }
    });

    REQUIRE(index.Size() == static_cast<size_t>(KEY_COUNT));

    std::set<KeyId> seenIds;
    for (const auto &k : keys)
    {
        const KeyId byFind = index.Find(k);
        REQUIRE(byFind != INVALID_KEY_ID);
        const KeyId byGetOrInsert = index.GetOrInsert(k);
        CHECK(byFind == byGetOrInsert);
        const auto [it, inserted] = seenIds.insert(byFind);
        INFO("Duplicate id " << byFind << " observed for key " << k);
        REQUIRE(inserted);
        CHECK(index.KeyOf(byFind) == k);
    }
    CHECK(*seenIds.begin() == 0);
    CHECK(*seenIds.rbegin() == static_cast<KeyId>(KEY_COUNT - 1));
    CHECK(index.Size() == static_cast<size_t>(KEY_COUNT));
}

// Regression: `KeyOf` previously read the `reverse` deque without a lock
// while `GetOrInsert` was emplace-ing into it. One writer grows the deque
// while four readers spin on `KeyOf` and round-trip each result through
// `Find`. Surfaces deterministically under TSan / Windows debug iterators.
TEST_CASE("KeyIndex KeyOf is safe to call while GetOrInsert grows the dictionary", "[key_index][stress]")
{
    constexpr int KEY_COUNT = 4'096;
    constexpr int READER_THREADS = 4;
    constexpr int READER_ITERATIONS = 50'000;

    std::vector<std::string> keys;
    keys.reserve(KEY_COUNT);
    for (int i = 0; i < KEY_COUNT; ++i)
    {
        keys.push_back("racer_key_" + std::to_string(i));
    }

    KeyIndex index;
    std::atomic<KeyId> highWater{0};
    std::atomic<bool> writerDone{false};

    std::thread writer([&] {
        for (const auto &k : keys)
        {
            // Publish ids only after the writer finishes inserting them.
            const KeyId id = index.GetOrInsert(k);
            highWater.store(id + 1, std::memory_order_release);
        }
        writerDone.store(true, std::memory_order_release);
    });

    auto reader = [&] {
        for (int i = 0; i < READER_ITERATIONS; ++i)
        {
            const KeyId limit = highWater.load(std::memory_order_acquire);
            if (limit == 0)
            {
                continue;
            }
            const KeyId id = static_cast<KeyId>(i) % limit;
            const std::string_view view = index.KeyOf(id);
            REQUIRE(index.Find(view) == id);
            if (writerDone.load(std::memory_order_acquire) && i > 1024)
            {
                break;
            }
        }
    };

    std::vector<std::thread> readers;
    readers.reserve(READER_THREADS);
    for (int t = 0; t < READER_THREADS; ++t)
    {
        readers.emplace_back(reader);
    }
    writer.join();
    for (auto &th : readers)
    {
        th.join();
    }

    REQUIRE(index.Size() == static_cast<size_t>(KEY_COUNT));
    for (KeyId id = 0; id < static_cast<KeyId>(KEY_COUNT); ++id)
    {
        const std::string_view view = index.KeyOf(id);
        CHECK(index.Find(view) == id);
    }
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
    constexpr char ALPHA_BUF[] = "alpha";
    constexpr char BETA_BUF[] = "beta";
    constexpr char ABSENT_BUF[] = "missing";
    const std::string_view alphaView(ALPHA_BUF, sizeof(ALPHA_BUF) - 1);
    const std::string_view betaView(BETA_BUF, sizeof(BETA_BUF) - 1);
    const std::string_view absentView(ABSENT_BUF, sizeof(ABSENT_BUF) - 1);

    CHECK(index.Find(alphaView) == alphaId);
    CHECK(index.Find(betaView) == betaId);
    CHECK(index.Find(absentView) == INVALID_KEY_ID);

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
