#include <loglib/tailing_bytes_producer.hpp>

#include <loglib_test/scaled_ms.hpp>

#include <catch2/catch_all.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

using loglib::TailingBytesProducer;
using loglib_test::ScaledMs;
using namespace std::chrono_literals;

namespace
{

/// Per-test scratch directory under `temp_directory_path()`; cleaned
/// up in the dtor. `temp_directory_path()` keeps parallel `ctest -j`
/// runs from colliding on a fixed filename.
class TempDir
{
public:
    TempDir()
    {
        auto base = std::filesystem::temp_directory_path();
        std::random_device rd;
        std::mt19937_64 gen(rd());
        const uint64_t suffix = gen();
        mPath = base / ("loglib_tail_test_" + std::to_string(suffix));
        std::filesystem::create_directories(mPath);
    }

    ~TempDir()
    {
        std::error_code ec;
        std::filesystem::remove_all(mPath, ec);
    }

    TempDir(const TempDir &) = delete;
    TempDir &operator=(const TempDir &) = delete;

    [[nodiscard]] std::filesystem::path File(const std::string &name) const
    {
        return mPath / name;
    }

    [[nodiscard]] const std::filesystem::path &Path() const noexcept
    {
        return mPath;
    }

private:
    std::filesystem::path mPath;
};

/// Append `text` to `path` (creating the file if missing) and flush.
void Append(const std::filesystem::path &path, std::string_view text)
{
    std::ofstream out(path, std::ios::binary | std::ios::app);
    REQUIRE(out.is_open());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
}

/// Overwrite the file with `text` (truncating to zero first if missing).
void Overwrite(const std::filesystem::path &path, std::string_view text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.flush();
}

/// Drain the source, parking on `WaitForBytes` between short reads.
/// Returns once `predicate(accumulated)` is true or `deadline`
/// elapses. The standard wait-for-output helper for these tests.
template <typename Predicate>
std::string DrainUntil(TailingBytesProducer &source, std::chrono::milliseconds deadline, Predicate predicate)
{
    std::string accumulated;
    const auto start = std::chrono::steady_clock::now();
    std::array<char, 4096> buf{};

    while (!source.IsClosed())
    {
        const auto now = std::chrono::steady_clock::now();
        if (now - start >= deadline)
        {
            break;
        }
        const size_t n = source.Read(std::span<char>(buf));
        if (n > 0)
        {
            accumulated.append(buf.data(), n);
            if (predicate(accumulated))
            {
                return accumulated;
            }
            continue;
        }
        if (predicate(accumulated))
        {
            return accumulated;
        }
        const auto remaining = deadline - (now - start);
        source.WaitForBytes(std::min<std::chrono::milliseconds>(
            std::chrono::duration_cast<std::chrono::milliseconds>(remaining), ScaledMs(25ms)
        ));
    }

    // Final drain after IsClosed.
    while (true)
    {
        const size_t n = source.Read(std::span<char>(buf));
        if (n == 0)
        {
            break;
        }
        accumulated.append(buf.data(), n);
    }
    return accumulated;
}

/// Splits `text` at `\n` and returns the resulting lines (without the
/// terminating `\n`). An empty trailing element (file ends with `\n`)
/// is dropped, so the caller can compare against a list of "logical"
/// lines.
std::vector<std::string> SplitLines(std::string_view text)
{
    std::vector<std::string> lines;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '\n')
        {
            lines.emplace_back(text.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < text.size())
    {
        lines.emplace_back(text.substr(start));
    }
    return lines;
}

/// Test defaults: no native watcher; short polling cadence (10x
/// faster than production) so test wall-clocks stay small.
TailingBytesProducer::Options FastPollOptions()
{
    TailingBytesProducer::Options options;
    options.disableNativeWatcher = true;
    options.pollInterval = 25ms;
    options.rotationDebounce = 250ms;
    options.readChunkBytes = 4 * 1024;
    options.prefillChunkBytes = 4 * 1024;
    return options;
}

/// Block until `source.RotationCount() >= target` or `deadline`
/// elapses. Tests use this to synchronise on rotation observation
/// instead of guessing how long a busy runner takes to schedule the
/// worker thread between two filesystem operations.
[[nodiscard]] bool WaitForRotationCount(
    const TailingBytesProducer &source, size_t target, std::chrono::milliseconds deadline
)
{
    const auto start = std::chrono::steady_clock::now();
    while (source.RotationCount() < target)
    {
        if (std::chrono::steady_clock::now() - start >= deadline)
        {
            return false;
        }
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

} // namespace

TEST_CASE("TailingBytesProducer pre-fill of last N complete lines on a small file", "[TailingBytesProducer]")
{
    TempDir dir;
    const auto path = dir.File("preset_lines.log");

    std::string content;
    for (int i = 0; i < 100; ++i)
    {
        content += "line " + std::to_string(i) + "\n";
    }
    Overwrite(path, content);

    TailingBytesProducer source(path, /*retentionLines=*/20, FastPollOptions());

    const std::string drained = DrainUntil(source, ScaledMs(1000ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= 20;
    });

    const auto lines = SplitLines(drained);
    REQUIRE(lines.size() == 20);
    CHECK(lines.front() == "line 80");
    CHECK(lines.back() == "line 99");
    // The producer canonicalises its input path (so the watcher parent
    // is always absolute), which on Windows can also resolve 8.3 short
    // names (e.g. `RUNNER~1` -> `runneradmin`) when the OS-supplied
    // `temp_directory_path()` returns a short form. Compare against the
    // canonicalised form so the test is robust on those CI runners.
    std::error_code ec;
    auto expected = std::filesystem::weakly_canonical(path, ec);
    if (ec || expected.empty())
    {
        expected = path;
    }
    CHECK(source.DisplayName() == expected.string());
}

TEST_CASE("TailingBytesProducer pre-fill on a file shorter than N", "[TailingBytesProducer]")
{
    TempDir dir;
    const auto path = dir.File("short.log");
    Overwrite(path, "a\nb\nc\n");

    TailingBytesProducer source(path, /*retentionLines=*/100, FastPollOptions());

    const std::string drained = DrainUntil(source, ScaledMs(500ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= 3;
    });

    const auto lines = SplitLines(drained);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "a");
    CHECK(lines[1] == "b");
    CHECK(lines[2] == "c");
}

// Regression: `Prefill` must `notify_all` after appending bytes,
// otherwise a parser parked in `WaitForBytes` sleeps until
// `pollInterval` elapses. We stretch `Prefill` with many small
// backwards reads so the test thread reliably parks before the
// pre-fill bytes land — exposing a missing notify.
TEST_CASE("TailingBytesProducer Prefill notifies WaitForBytes immediately", "[TailingBytesProducer]")
{
    TempDir dir;
    const auto path = dir.File("notify_prefill.log");

    // ~500 KiB across 5 000 ~100-byte lines. With 512-byte backwards
    // chunks, `Prefill` runs ~900 syscalls before notifying — easily
    // enough for the test thread to win the race into `WaitForBytes`.
    std::string content;
    constexpr int LINE_COUNT = 5000;
    content.reserve(LINE_COUNT * 110);
    for (int i = 0; i < LINE_COUNT; ++i)
    {
        content += "line " + std::to_string(i) + std::string(95, 'x') + "\n";
    }
    Overwrite(path, content);

    auto opts = FastPollOptions();
    // pollInterval intentionally above any test deadline so a
    // missing `notify_all()` dominates the measurement (regression:
    // 10 s wait; fixed: I/O cost of `Prefill` only).
    opts.pollInterval = 10000ms;
    opts.prefillChunkBytes = 512;

    TailingBytesProducer source(path, /*retentionLines=*/LINE_COUNT - 1, opts);

    const auto waitStart = std::chrono::steady_clock::now();
    source.WaitForBytes(ScaledMs(3000ms));
    const auto waitElapsed = std::chrono::steady_clock::now() - waitStart;

    // 2s is generous for CI disks but well below the 10s regression.
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(waitElapsed) < ScaledMs(2000ms));

    // Verify the wake wasn't spurious by draining the expected lines.
    const std::string drained = DrainUntil(source, ScaledMs(2000ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= LINE_COUNT - 1;
    });
    const auto lines = SplitLines(drained);
    CHECK(lines.size() == static_cast<size_t>(LINE_COUNT - 1));
}

TEST_CASE(
    "TailingBytesProducer pre-fill aborts gracefully on a file with no newlines within the scan budget",
    "[TailingBytesProducer]"
)
{
    // 2 MiB file with no newlines and a 128 KiB scan budget must not
    // wedge the ctor: pre-fill aborts, yields zero lines, seeks to
    // EOF, and future appends still appear in the tail.
    TempDir dir;
    const auto path = dir.File("huge_line.log");
    {
        std::ofstream out(path, std::ios::binary);
        REQUIRE(out.is_open());
        const std::string blob(2 * 1024 * 1024, 'x'); // 2 MiB, no '\n'
        out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    }

    auto opts = FastPollOptions();
    opts.prefillMaxScanBytes = 128 * 1024; // well below the file size
    opts.prefillChunkBytes = 32 * 1024;    // smaller than the budget

    const auto ctorStart = std::chrono::steady_clock::now();
    TailingBytesProducer source(path, /*retentionLines=*/100, opts);
    const auto ctorElapsed = std::chrono::steady_clock::now() - ctorStart;
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(ctorElapsed) < ScaledMs(500ms));

    // No pre-fill lines should surface; the brief sleep gives any
    // spurious bytes time to land before we sample.
    std::this_thread::sleep_for(ScaledMs(50ms));
    std::array<char, 1024> buf{};
    const size_t drained = source.Read(std::span<char>(buf));
    CHECK(drained == 0);

    // Tailing still works: appending a complete line produces it.
    Append(path, "\nafter\n");
    const auto post = DrainUntil(source, ScaledMs(2000ms), [](const std::string &acc) {
        return acc.find("after\n") != std::string::npos;
    });
    CHECK(post.find("after\n") != std::string::npos);
    // The 2 MiB "xxx…" prefix must NOT have been synthesized into a line.
    CHECK(post.find("xxxxx") == std::string::npos);
}

TEST_CASE("TailingBytesProducer detects growth across many small writes", "[TailingBytesProducer]")
{
    TempDir dir;
    const auto path = dir.File("growth.log");
    Overwrite(path, ""); // start empty

    TailingBytesProducer source(path, /*retentionLines=*/10, FastPollOptions());

    constexpr int COUNT = 25;
    std::thread writer([&] {
        for (int i = 0; i < COUNT; ++i)
        {
            Append(path, "g" + std::to_string(i) + "\n");
            std::this_thread::sleep_for(ScaledMs(5ms));
        }
    });

    const std::string drained = DrainUntil(source, ScaledMs(2000ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= COUNT;
    });
    writer.join();

    const auto lines = SplitLines(drained);
    REQUIRE(lines.size() >= static_cast<size_t>(COUNT));
    for (int i = 0; i < COUNT; ++i)
    {
        CHECK(lines[i] == "g" + std::to_string(i));
    }
}

TEST_CASE("TailingBytesProducer recovers from rename-and-create rotation", "[TailingBytesProducer][rotation]")
{
    TempDir dir;
    const auto path = dir.File("rotate.log");
    const auto rotatedPath = dir.File("rotate.log.1");
    Overwrite(path, "pre1\npre2\n");

    std::atomic<int> rotationFires{0};
    TailingBytesProducer source(path, /*retentionLines=*/100, FastPollOptions());
    source.SetRotationCallback([&] { rotationFires.fetch_add(1, std::memory_order_release); });

    // Drain pre-rotation lines.
    auto prefilled = DrainUntil(source, ScaledMs(500ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= 2;
    });
    auto preLines = SplitLines(prefilled);
    REQUIRE(preLines.size() == 2);
    CHECK(preLines[0] == "pre1");
    CHECK(preLines[1] == "pre2");

    // Rename then create a fresh file at the original path.
    std::filesystem::rename(path, rotatedPath);
    Overwrite(path, "post1\npost2\n");

    const auto post = DrainUntil(source, ScaledMs(2000ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= 2;
    });
    const auto postLines = SplitLines(post);
    REQUIRE(postLines.size() >= 2);
    CHECK(postLines[0] == "post1");
    CHECK(postLines[1] == "post2");
    CHECK(rotationFires.load(std::memory_order_acquire) >= 1);
    CHECK(source.RotationCount() >= 1);
}

TEST_CASE("TailingBytesProducer recovers from copytruncate rotation", "[TailingBytesProducer][rotation]")
{
    TempDir dir;
    const auto path = dir.File("copytrunc.log");
    Overwrite(path, "old1\nold2\nold3\n");

    std::atomic<int> rotationFires{0};
    TailingBytesProducer source(path, /*retentionLines=*/100, FastPollOptions());
    source.SetRotationCallback([&] { rotationFires.fetch_add(1, std::memory_order_release); });

    // Drain pre-rotation.
    DrainUntil(source, ScaledMs(500ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= 3;
    });

    // Simulate copytruncate: copy elsewhere (we ignore the copy), then
    // truncate in place.
    Overwrite(path, "");
    std::this_thread::sleep_for(ScaledMs(50ms)); // give the worker a poll tick to detect
    Append(path, "new1\nnew2\n");

    const auto post = DrainUntil(source, ScaledMs(2000ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= 2;
    });
    const auto postLines = SplitLines(post);
    REQUIRE(postLines.size() >= 2);
    CHECK(postLines[0] == "new1");
    CHECK(postLines[1] == "new2");
    CHECK(rotationFires.load(std::memory_order_acquire) >= 1);
}

TEST_CASE("TailingBytesProducer recovers from in-place truncate", "[TailingBytesProducer][rotation]")
{
    TempDir dir;
    const auto path = dir.File("inplace_trunc.log");
    Overwrite(path, "x1\nx2\nx3\n");

    TailingBytesProducer source(path, /*retentionLines=*/100, FastPollOptions());

    DrainUntil(source, ScaledMs(500ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= 3;
    });

    // Truncate in place via overwrite (`: > path` equivalent).
    Overwrite(path, "");
    std::this_thread::sleep_for(ScaledMs(50ms));
    Append(path, "y1\n");

    const auto post = DrainUntil(source, ScaledMs(2000ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= 1;
    });
    const auto postLines = SplitLines(post);
    REQUIRE(postLines.size() >= 1);
    CHECK(postLines[0] == "y1");
    CHECK(source.RotationCount() >= 1);
}

TEST_CASE(
    "TailingBytesProducer keeps ingesting while the consumer is paused (rotation included)",
    "[TailingBytesProducer][rotation]"
)
{
    TempDir dir;
    const auto path = dir.File("paused.log");
    Overwrite(path, "p1\np2\n");

    TailingBytesProducer source(path, /*retentionLines=*/100, FastPollOptions());

    // The "consumer" never calls Read while we rotate; ingestion (and
    // rotation detection) must still happen on the worker.
    std::this_thread::sleep_for(ScaledMs(50ms)); // let pre-fill settle
    Overwrite(path, "");                         // truncate
    // Synchronise on rotation observation. Without this, the new
    // file ("q1\nq2\n", 6 bytes) and the original ("p1\np2\n", 6
    // bytes) are byte-for-byte the same length, so a busy macOS CI
    // runner that misses the size-0 poll window would never see the
    // rotation branch fire.
    REQUIRE(WaitForRotationCount(source, 1, ScaledMs(2000ms)));
    Append(path, "q1\nq2\n");
    std::this_thread::sleep_for(ScaledMs(100ms)); // poll tick + read

    const auto drained = DrainUntil(source, ScaledMs(1000ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= 4;
    });
    const auto lines = SplitLines(drained);
    REQUIRE(lines.size() >= 4);
    CHECK(lines[0] == "p1");
    CHECK(lines[1] == "p2");
    CHECK(lines[2] == "q1");
    CHECK(lines[3] == "q2");
}

TEST_CASE("TailingBytesProducer discards partial line on rotation", "[TailingBytesProducer][rotation]")
{
    TempDir dir;
    const auto path = dir.File("partial_rot.log");
    // Note: no trailing newline → "incomplete" stays in the partial-line buffer.
    Overwrite(path, "complete\nincomplete");

    TailingBytesProducer source(path, /*retentionLines=*/100, FastPollOptions());

    // Drain "complete" first.
    auto first = DrainUntil(source, ScaledMs(500ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= 1;
    });
    auto firstLines = SplitLines(first);
    REQUIRE(firstLines.size() >= 1);
    CHECK(firstLines[0] == "complete");

    // Rotate via in-place truncate. The partial "incomplete" must be
    // discarded.
    Overwrite(path, "");
    std::this_thread::sleep_for(ScaledMs(50ms));
    Append(path, "after\n");

    const auto post = DrainUntil(source, ScaledMs(1000ms), [](const std::string &acc) {
        return acc.find("after\n") != std::string::npos;
    });
    const auto postLines = SplitLines(post);
    REQUIRE(postLines.size() >= 1);
    CHECK(postLines.back() == "after");
    // The partial "incomplete" must NEVER appear.
    CHECK(post.find("incomplete") == std::string::npos);
}

TEST_CASE("TailingBytesProducer flushes the partial line on Stop", "[TailingBytesProducer]")
{
    TempDir dir;
    const auto path = dir.File("partial_stop.log");
    Overwrite(path, "first\ntrailing-no-newline");

    TailingBytesProducer source(path, /*retentionLines=*/100, FastPollOptions());

    auto pre = DrainUntil(source, ScaledMs(500ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= 1;
    });
    auto preLines = SplitLines(pre);
    REQUIRE(preLines.size() >= 1);
    CHECK(preLines[0] == "first");

    // Now Stop. The partial-line buffer must be flushed as a synthetic
    // last line.
    source.Stop();

    auto post = DrainUntil(source, ScaledMs(1000ms), [&](const std::string & /*acc*/) { return source.IsClosed(); });
    const auto postLines = SplitLines(post);
    REQUIRE(postLines.size() >= 1);
    CHECK(postLines.back() == "trailing-no-newline");
    CHECK(source.IsClosed());
}

TEST_CASE("TailingBytesProducer Stop unblocks WaitForBytes parked on an idle file", "[TailingBytesProducer]")
{
    TempDir dir;
    const auto path = dir.File("idle.log");
    Overwrite(path, ""); // empty; worker will park waiting for growth

    TailingBytesProducer source(path, /*retentionLines=*/100, FastPollOptions());

    std::atomic<bool> waitReturned{false};
    const auto waitStart = std::chrono::steady_clock::now();
    std::thread waiter([&] {
        source.WaitForBytes(5s);
        waitReturned.store(true, std::memory_order_release);
    });

    // Give the waiter a tick to enter `WaitForBytes`, then signal Stop.
    std::this_thread::sleep_for(ScaledMs(50ms));
    source.Stop();
    waiter.join();
    const auto elapsed = std::chrono::steady_clock::now() - waitStart;

    CHECK(waitReturned.load(std::memory_order_acquire));
    // 500 ms budget for stop+I/O unwind; idle case is well under.
    CHECK(elapsed < ScaledMs(500ms));
}

TEST_CASE("TailingBytesProducer debounces rapid rotations within 1 s", "[TailingBytesProducer][rotation]")
{
    TempDir dir;
    const auto path = dir.File("debounce.log");
    Overwrite(path, "seed\n");

    auto opts = FastPollOptions();
    // Long debounce so we can verify three rotations inside one window;
    // 5 ms polls so the worker reliably observes every identity flip.
    opts.rotationDebounce = 1000ms;
    opts.pollInterval = 5ms;

    std::atomic<int> rotationFires{0};
    TailingBytesProducer source(path, /*retentionLines=*/100, opts);
    source.SetRotationCallback([&] { rotationFires.fetch_add(1, std::memory_order_release); });

    // Wait for pre-fill to consume the seed and for the worker to settle
    // on a steady mReadOffset.
    DrainUntil(source, ScaledMs(500ms), [](const std::string &acc) { return acc.find("seed\n") != std::string::npos; });

    // Rename-and-create rotations flip the path's inode; identity
    // detection (branch i) is state-based, so the worker reliably
    // observes every flip -- provided we give it a poll window between
    // successive flips. A fixed ms budget is not enough on slow CI
    // hosts (observed: macOS arm64 runners intermittently miss an
    // intermediate flip when the worker loses a scheduling slot to
    // the filesystem ops on this thread; the path's inode ends up
    // jumping from rotation i-1 straight to rotation i+1 and only
    // one rotation is counted). Synchronise on the observed
    // `RotationCount()` instead. The per-step deadline is much
    // smaller than `rotationDebounce` so all three rotations still
    // land inside the same debounce window, preserving the
    // callback-coalescing assertion below.
    for (int i = 0; i < 3; ++i)
    {
        const auto rotated = dir.File("debounce.log." + std::to_string(i));
        std::filesystem::rename(path, rotated);
        Overwrite(path, "rot" + std::to_string(i) + "\n");
        REQUIRE(WaitForRotationCount(source, static_cast<size_t>(i + 1), ScaledMs(250ms)));
    }

    // Drain so we know the worker has processed at least the last rotation.
    DrainUntil(source, ScaledMs(1000ms), [](const std::string &acc) {
        return acc.find("rot2\n") != std::string::npos;
    });

    // RotationCount counts every detected rotation; the callback
    // collapses them into one per debounce window.
    CHECK(source.RotationCount() >= 3);
    CHECK(rotationFires.load(std::memory_order_acquire) == 1);
}

TEST_CASE("TailingBytesProducer throws on initial open failure", "[TailingBytesProducer]")
{
    TempDir dir;
    const auto path = dir.File("does_not_exist.log");

    CHECK_THROWS_AS(TailingBytesProducer(path, 100, FastPollOptions()), std::system_error);
}

TEST_CASE("TailingBytesProducer Stop after natural drain leaves IsClosed true", "[TailingBytesProducer]")
{
    TempDir dir;
    const auto path = dir.File("simple.log");
    Overwrite(path, "only\n");

    TailingBytesProducer source(path, /*retentionLines=*/100, FastPollOptions());
    auto drained = DrainUntil(source, ScaledMs(500ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= 1;
    });
    CHECK(SplitLines(drained).front() == "only");

    source.Stop();
    // Allow the worker time to observe the stop flag and exit.
    auto deadline = std::chrono::steady_clock::now() + ScaledMs(500ms);
    while (!source.IsClosed() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(ScaledMs(5ms));
    }
    CHECK(source.IsClosed());

    // Subsequent Read returns 0 with IsClosed true.
    std::array<char, 8> buf{};
    CHECK(source.Read(std::span<char>(buf)) == 0);
    CHECK(source.IsClosed());
}

TEST_CASE(
    "TailingBytesProducer reports SourceStatus::Waiting while the file is missing", "[TailingBytesProducer][status]"
)
{
    // When the watched file disappears the source transitions to
    // `Waiting`; a subsequent re-create lifts it back to `Running`.
    // Edge-triggered: one delete/create pair => one `Waiting` and
    // one `Running` observation.
    TempDir dir;
    const auto path = dir.File("status.log");
    Overwrite(path, "seed\n");

    std::mutex statusMu;
    std::vector<loglib::SourceStatus> observed;

    TailingBytesProducer source(path, /*retentionLines=*/100, FastPollOptions());
    source.SetStatusCallback([&](loglib::SourceStatus s) {
        std::lock_guard<std::mutex> lock(statusMu);
        observed.push_back(s);
    });

    // Drain the pre-fill so the worker is in its steady-state tail loop
    // before we perturb the file.
    DrainUntil(source, ScaledMs(500ms), [](const std::string &acc) { return acc.find("seed\n") != std::string::npos; });

    // Delete the file → branch (ii) → Waiting.
    std::filesystem::remove(path);

    auto waitingDeadline = std::chrono::steady_clock::now() + ScaledMs(2000ms);
    while (std::chrono::steady_clock::now() < waitingDeadline)
    {
        {
            std::lock_guard<std::mutex> lock(statusMu);
            if (!observed.empty() && observed.back() == loglib::SourceStatus::Waiting)
            {
                break;
            }
        }
        std::array<char, 256> buf{};
        source.Read(std::span<char>(buf));
        std::this_thread::sleep_for(ScaledMs(25ms));
    }

    {
        std::lock_guard<std::mutex> lock(statusMu);
        REQUIRE_FALSE(observed.empty());
        CHECK(observed.front() == loglib::SourceStatus::Waiting);
    }

    // Re-create the file — branch (i) / (ii-reopened) → Running.
    Overwrite(path, "after\n");

    auto runningDeadline = std::chrono::steady_clock::now() + ScaledMs(2000ms);
    while (std::chrono::steady_clock::now() < runningDeadline)
    {
        {
            std::lock_guard<std::mutex> lock(statusMu);
            if (!observed.empty() && observed.back() == loglib::SourceStatus::Running)
            {
                break;
            }
        }
        std::array<char, 256> buf{};
        source.Read(std::span<char>(buf));
        std::this_thread::sleep_for(ScaledMs(25ms));
    }

    std::lock_guard<std::mutex> lock(statusMu);
    REQUIRE(observed.size() >= 2);
    CHECK(observed.back() == loglib::SourceStatus::Running);

    // Edge-triggered: no duplicate Running→Running or Waiting→Waiting
    // emissions.
    for (size_t i = 1; i < observed.size(); ++i)
    {
        CHECK(observed[i] != observed[i - 1]);
    }
}
