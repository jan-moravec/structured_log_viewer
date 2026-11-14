#include "common.hpp"

#include <loglib/core/bytes_producer.hpp>
#include <loglib/core/line_source.hpp>
#include <loglib/core/stream_line_source.hpp>

#include <catch2/catch_all.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using loglib::StreamLineSource;

namespace
{

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

TEST_CASE("StreamLineSource: ResolveMmapBytes resolves slices of the raw line", "[StreamLineSource]")
{
    StreamLineSource source(std::filesystem::path("s.log"), nullptr);
    const size_t id = source.AppendLine(R"({"msg":"hello"})", "");

    CHECK(source.ResolveMmapBytes(8, 5, id) == "hello");
    CHECK_FALSE(source.SlicesOutliveRows());

    // A range past the end of the line, or a line that does not exist, gives an empty view and not a partial one.
    CHECK(source.ResolveMmapBytes(8, 64, id).empty());
    CHECK(source.ResolveMmapBytes(0, 1, id + 1).empty());
}

TEST_CASE("StreamLineSource: ReadBatch holds the lock for the reads of a chunk", "[StreamLineSource]")
{
    StreamLineSource source(std::filesystem::path("s.log"), nullptr);
    const size_t id = source.AppendLine("raw", "payload");

    std::atomic<bool> appended{false};
    std::thread writer;
    {
        const StreamLineSource::ReadBatch batch(&source);
        // A second batch for the same source on the same thread takes no second lock, and a null source is allowed.
        const StreamLineSource::ReadBatch nested(&source);
        const StreamLineSource::ReadBatch none(nullptr);

        CHECK(source.ResolveOwnedBytes(0, 7, id) == "payload");
        CHECK(source.ResolveMmapBytes(0, 3, id) == "raw");
        CHECK(source.Size() == 1);

        writer = std::thread([&] {
            source.AppendLine("next", "");
            appended = true;
        });
        // The append waits for the batch to end.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK_FALSE(appended.load());
    }
    writer.join();
    CHECK(appended.load());
    CHECK(source.Size() == 2);
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
    auto producer = std::make_unique<InMemoryBytesProducer>("abc");
    InMemoryBytesProducer *raw = producer.get();

    StreamLineSource source(std::filesystem::path("s.log"), std::move(producer));

    CHECK(source.Producer() == raw);

    const auto &constSource = source;
    CHECK(constSource.Producer() == raw);

    // Round-trip a couple of producer methods through the source to
    // confirm the wiring is live (real parser code would use this seam).
    std::array<char, 8> buffer{};
    CHECK(source.Producer()->Read(buffer) == 3);
    CHECK(std::string_view(buffer.data(), 3) == "abc");
}

TEST_CASE("StreamLineSource: Producer() is null when constructed without one", "[StreamLineSource]")
{
    StreamLineSource source(std::filesystem::path("s.log"), nullptr);
    CHECK(source.Producer() == nullptr);
}

TEST_CASE("Retention bounds stay ordered", "[line_source]")
{
    CHECK(loglib::MIN_RETENTION_LINES <= loglib::DEFAULT_RETENTION_LINES);
    CHECK(loglib::DEFAULT_RETENTION_LINES <= loglib::MAX_RETENTION_LINES);
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
