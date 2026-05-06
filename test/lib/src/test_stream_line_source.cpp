#include <loglib/bytes_producer.hpp>
#include <loglib/stream_line_source.hpp>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

using loglib::BytesProducer;
using loglib::StreamLineSource;

namespace
{

/// Minimal `BytesProducer` test double. Only the few entry points
/// `StreamLineSource` exposes through `Producer()` are exercised — the
/// goal is to confirm wiring, not to re-test `BytesProducer`.
class FakeProducer final : public BytesProducer
{
public:
    size_t Read(std::span<char> /*buffer*/) override
    {
        ++mReadCalls;
        return 0;
    }

    void WaitForBytes(std::chrono::milliseconds /*timeout*/) override
    {
        ++mWaitCalls;
    }

    void Stop() noexcept override
    {
        mStopped = true;
    }

    [[nodiscard]] bool IsClosed() const noexcept override
    {
        return mStopped;
    }

    [[nodiscard]] std::string DisplayName() const override
    {
        return "fake-producer";
    }

    size_t mReadCalls = 0;
    size_t mWaitCalls = 0;
    std::atomic<bool> mStopped{false};
};

} // namespace

TEST_CASE("StreamLineSource: capability flags", "[StreamLineSource]")
{
    StreamLineSource source(std::filesystem::path("stream.log"), nullptr);

    CHECK_FALSE(source.BytesAreStable());
    CHECK(source.SupportsEviction());
    CHECK(source.Path() == std::filesystem::path("stream.log"));
    CHECK(source.FirstAvailableLineId() == 1);
    CHECK(source.Size() == 0);
}

TEST_CASE("StreamLineSource: AppendLine assigns 1-based monotonic ids", "[StreamLineSource]")
{
    StreamLineSource source(std::filesystem::path("s.log"), nullptr);

    const size_t a = source.AppendLine("line a", "");
    const size_t b = source.AppendLine("line b", "");
    const size_t c = source.AppendLine("line c", "");

    CHECK(a == 1);
    CHECK(b == 2);
    CHECK(c == 3);
    CHECK(source.Size() == 3);

    CHECK(source.RawLine(1) == "line a");
    CHECK(source.RawLine(2) == "line b");
    CHECK(source.RawLine(3) == "line c");

    CHECK_THROWS_AS(source.RawLine(0), std::out_of_range);
    CHECK_THROWS_AS(source.RawLine(4), std::out_of_range);
}

TEST_CASE("StreamLineSource: ResolveOwnedBytes resolves per-line arenas", "[StreamLineSource]")
{
    StreamLineSource source(std::filesystem::path("s.log"), nullptr);

    // Per-line owned-bytes arena: the parser writes escape-decoded bytes
    // into a per-line scratch and stamps `OwnedString` payloads with
    // offsets into it. `StreamLineSource` stores that scratch verbatim.
    const std::string lineOneArena = "alphabeta";         // 0..4 = "alpha", 5..8 = "beta"
    const std::string lineTwoArena = "gammadeltaepsilon"; // 0..4 = "gamma", 5..9 = "delta", 10..16 = "epsilon"

    const size_t id1 = source.AppendLine("raw 1", lineOneArena);
    const size_t id2 = source.AppendLine("raw 2", lineTwoArena);

    CHECK(source.ResolveOwnedBytes(0, 5, id1) == "alpha");
    CHECK(source.ResolveOwnedBytes(5, 4, id1) == "beta");
    CHECK(source.ResolveOwnedBytes(0, 5, id2) == "gamma");
    CHECK(source.ResolveOwnedBytes(5, 5, id2) == "delta");
    CHECK(source.ResolveOwnedBytes(10, 7, id2) == "epsilon");

    // Resolving across line boundaries is not allowed: each id has its
    // own private arena.
    CHECK(source.ResolveOwnedBytes(0, 5, /*lineId=*/0).empty());
    CHECK(source.ResolveOwnedBytes(0, 5, /*lineId=*/3).empty());

    // Out-of-range payloads return an empty view, not a partial one.
    CHECK(source.ResolveOwnedBytes(0, 1024, id1).empty());
    CHECK(source.ResolveOwnedBytes(lineOneArena.size(), 1, id1).empty());
}

TEST_CASE("StreamLineSource: ResolveMmapBytes always returns empty (defensive)", "[StreamLineSource]")
{
    StreamLineSource source(std::filesystem::path("s.log"), nullptr);
    source.AppendLine("raw", "payload");

    // Stream sources never produce MmapSlice compact values. The
    // resolver returns an empty view if a stale value somehow reaches
    // it — defensive, not a correctness path.
    CHECK(source.ResolveMmapBytes(0, 7, /*lineId=*/1).empty());
}

TEST_CASE("StreamLineSource: EvictBefore drops prefix lines", "[StreamLineSource]")
{
    StreamLineSource source(std::filesystem::path("s.log"), nullptr);

    source.AppendLine("a", "payloadA");
    source.AppendLine("b", "payloadB");
    source.AppendLine("c", "payloadC");
    source.AppendLine("d", "payloadD");
    REQUIRE(source.Size() == 4);

    source.EvictBefore(3);

    CHECK(source.FirstAvailableLineId() == 3);
    CHECK(source.Size() == 2);

    // Surviving lines still resolve correctly (raw text + owned arena).
    CHECK(source.RawLine(3) == "c");
    CHECK(source.RawLine(4) == "d");
    CHECK(source.ResolveOwnedBytes(0, 8, /*lineId=*/3) == "payloadC");
    CHECK(source.ResolveOwnedBytes(0, 8, /*lineId=*/4) == "payloadD");

    // Evicted ids no longer resolve.
    CHECK_THROWS_AS(source.RawLine(1), std::out_of_range);
    CHECK_THROWS_AS(source.RawLine(2), std::out_of_range);
    CHECK(source.ResolveOwnedBytes(0, 8, /*lineId=*/1).empty());
    CHECK(source.ResolveOwnedBytes(0, 8, /*lineId=*/2).empty());

    // New appends continue from where the counter left off.
    const size_t e = source.AppendLine("e", "");
    CHECK(e == 5);
    CHECK(source.RawLine(5) == "e");
    CHECK(source.Size() == 3);
}

TEST_CASE("StreamLineSource: EvictBefore with id <= FirstAvailableLineId is a no-op", "[StreamLineSource]")
{
    StreamLineSource source(std::filesystem::path("s.log"), nullptr);
    source.AppendLine("a", "");
    source.AppendLine("b", "");

    source.EvictBefore(1);
    CHECK(source.FirstAvailableLineId() == 1);
    CHECK(source.Size() == 2);

    source.EvictBefore(0);
    CHECK(source.FirstAvailableLineId() == 1);
    CHECK(source.Size() == 2);
}

TEST_CASE("StreamLineSource: EvictBefore beyond NextLineId clamps at the tail", "[StreamLineSource]")
{
    StreamLineSource source(std::filesystem::path("s.log"), nullptr);
    source.AppendLine("a", "");
    source.AppendLine("b", "");
    source.AppendLine("c", "");

    // Over-shoot: the call is accepted and drops everything, but the
    // first-available id stays at the next-id watermark so subsequent
    // appends pick up cleanly.
    source.EvictBefore(1000);
    CHECK(source.FirstAvailableLineId() == 4);
    CHECK(source.Size() == 0);

    const size_t next = source.AppendLine("d", "");
    CHECK(next == 4);
    CHECK(source.RawLine(4) == "d");
}

TEST_CASE("StreamLineSource: Producer() returns the underlying BytesProducer", "[StreamLineSource]")
{
    auto producer = std::make_unique<FakeProducer>();
    FakeProducer *raw = producer.get();

    StreamLineSource source(std::filesystem::path("s.log"), std::move(producer));

    CHECK(source.Producer() == raw);

    const auto &constSource = source;
    CHECK(constSource.Producer() == raw);

    // Round-trip a couple of producer methods through the source to
    // confirm the wiring is live (real parser code would use this seam).
    const std::span<char> buffer;
    CHECK(source.Producer()->Read(buffer) == 0);
    CHECK(raw->mReadCalls == 1);
}

TEST_CASE("StreamLineSource: Producer() is null when constructed without one", "[StreamLineSource]")
{
    StreamLineSource source(std::filesystem::path("s.log"), nullptr);
    CHECK(source.Producer() == nullptr);
}

TEST_CASE("StreamLineSource: OwnedMemoryBytes accounts for line text and per-line arenas", "[StreamLineSource]")
{
    StreamLineSource emptySource(std::filesystem::path("s.log"), nullptr);
    const size_t emptyBytes = emptySource.OwnedMemoryBytes();

    StreamLineSource source(std::filesystem::path("s.log"), nullptr);
    source.AppendLine(std::string(1024, 'a'), std::string(2048, 'b'));
    source.AppendLine(std::string(512, 'c'), std::string(256, 'd'));

    const size_t bytesAfter = source.OwnedMemoryBytes();
    CHECK(bytesAfter > emptyBytes);
    // Lower bound: payload bytes alone (1024 + 2048 + 512 + 256 = 3840).
    CHECK(bytesAfter >= 3840);
}
