#include <loglib/ingest/internal/line_bytes_queue.hpp>

#include <catch2/catch_all.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

using loglib::internal::LineBytesQueue;

namespace
{

std::string Drain(LineBytesQueue &queue)
{
    std::string out;
    std::array<char, 7> chunk{};
    while (const std::size_t got = queue.Read(chunk))
    {
        out.append(chunk.data(), got);
    }
    return out;
}

} // namespace

TEST_CASE("LineBytesQueue starts empty and reads nothing", "[line_bytes_queue]")
{
    LineBytesQueue queue;
    std::array<char, 4> buffer{};
    CHECK(queue.Empty());
    CHECK(queue.Size() == 0);
    CHECK(queue.Read(buffer) == 0);
}

TEST_CASE("LineBytesQueue returns bytes in order and honours the read size", "[line_bytes_queue]")
{
    LineBytesQueue queue;
    std::atomic<std::size_t> dropped{0};
    queue.Append(std::string_view("hello\nworld\n"), 0, dropped);
    CHECK(queue.Size() == 12);

    std::array<char, 4> buffer{};
    REQUIRE(queue.Read(buffer) == 4);
    CHECK(std::string_view(buffer.data(), 4) == "hell");
    CHECK(queue.Size() == 8);
    CHECK(Drain(queue) == "o\nworld\n");
    CHECK(queue.Empty());
    CHECK(dropped.load() == 0);

    CHECK(queue.Read(std::span<char>{}) == 0);
}

TEST_CASE("LineBytesQueue ignores an empty append", "[line_bytes_queue]")
{
    LineBytesQueue queue;
    std::atomic<std::size_t> dropped{0};
    queue.Append(std::string_view(), 4, dropped);
    queue.Append(nullptr, 0, 4, dropped);
    CHECK(queue.Empty());
    CHECK(dropped.load() == 0);
}

TEST_CASE("LineBytesQueue with a cap of zero never drops", "[line_bytes_queue]")
{
    LineBytesQueue queue;
    std::atomic<std::size_t> dropped{0};
    for (int i = 0; i < 100; ++i)
    {
        queue.Append(std::string_view("0123456789\n"), 0, dropped);
    }
    CHECK(queue.Size() == 1100);
    CHECK(dropped.load() == 0);
}

TEST_CASE("LineBytesQueue drops whole lines, so a reader never sees a torn line", "[line_bytes_queue]")
{
    LineBytesQueue queue;
    std::atomic<std::size_t> dropped{0};
    queue.Append(std::string_view("aaaa\nbbbb\ncccc\n"), 20, dropped);
    REQUIRE(queue.Size() == 15);

    // 15 + 5 is exactly the cap, so nothing is dropped yet.
    queue.Append(std::string_view("dddd\n"), 20, dropped);
    CHECK(queue.Size() == 20);
    CHECK(dropped.load() == 0);

    // Five more bytes need five dropped. That ends inside "bbbb", so the drop runs on to the end of that line.
    queue.Append(std::string_view("eeee\n"), 20, dropped);
    CHECK(dropped.load() == 10);
    CHECK(Drain(queue) == "cccc\ndddd\neeee\n");
}

TEST_CASE("LineBytesQueue rounds a drop up to the next newline", "[line_bytes_queue]")
{
    LineBytesQueue queue;
    std::atomic<std::size_t> dropped{0};
    queue.Append(std::string_view("aaaaaaaa\nbb\n"), 14, dropped);
    // Two bytes over: the drop starts inside the first line and ends after its newline.
    queue.Append(std::string_view("cc\nX"), 14, dropped);
    CHECK(dropped.load() == 9);
    CHECK(Drain(queue) == "bb\ncc\nX");
}

TEST_CASE("LineBytesQueue drops everything queued when no newline follows the drop point", "[line_bytes_queue]")
{
    LineBytesQueue queue;
    std::atomic<std::size_t> dropped{0};
    queue.Append(std::string_view("no newline here"), 16, dropped);
    queue.Append(std::string_view("more\n"), 16, dropped);
    CHECK(dropped.load() == 15);
    CHECK(Drain(queue) == "more\n");
}

TEST_CASE("LineBytesQueue keeps an oversized append and drops what was queued", "[line_bytes_queue]")
{
    LineBytesQueue queue;
    std::atomic<std::size_t> dropped{0};
    queue.Append(std::string_view("old\n"), 8, dropped);
    queue.Append(std::string_view("a much longer payload\n"), 8, dropped);
    CHECK(dropped.load() == 4);
    CHECK(Drain(queue) == "a much longer payload\n");
}

TEST_CASE("LineBytesQueue adds up dropped bytes across appends", "[line_bytes_queue]")
{
    LineBytesQueue queue;
    std::atomic<std::size_t> dropped{0};
    for (int i = 0; i < 10; ++i)
    {
        queue.Append(std::string_view("12345678\n"), 20, dropped);
    }
    // Each line is 9 bytes. Ten were written and the queue holds whole lines only.
    CHECK(queue.Size() + dropped.load() == 90);
    CHECK(queue.Size() <= 20);
    CHECK(Drain(queue).size() % 9 == 0);
}
