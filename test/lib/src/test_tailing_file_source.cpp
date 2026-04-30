#include <loglib/tailing_file_source.hpp>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include <string>
#include <thread>
#include <vector>

using loglib::TailingFileSource;
using namespace std::chrono_literals;

namespace
{

/// Read-once test-time multiplier for polling-fallback waits. Slow CI
/// runners (Linux containerised, Windows MSVC matrix, macOS shared
/// arm runners) can blow past the wall-clock budgets the harness picks
/// for a developer-class workstation; rather than scattering ad-hoc
/// `*= 2` factors through every `DrainUntil` call we read the
/// `LOGLIB_TEST_TIME_SCALE` env var once, parse it as a double (default
/// `1.0`), and multiply every test-only deadline through `Scaled` /
/// `ScaledMs` (PRD §7 *CI*, task 6.1). `pollInterval` itself stays
/// fixed at 25 ms because slowing the polls down would defeat the
/// purpose of the harness — only the *deadlines waiting for the worker
/// to do its thing* scale.
double LoadTimeScale()
{
    // MSVC flags `std::getenv` as `unsafe` (C4996) and recommends `_dupenv_s`;
    // the value here is read once at process start and treated as untrusted
    // input (parsed via `strtod`, default-on-failure), so the safer-API
    // pattern would only add noise. Suppress the warning locally rather
    // than blanket-disable C4996 for the TU.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char *raw = std::getenv("LOGLIB_TEST_TIME_SCALE");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    if (!raw || !*raw)
    {
        return 1.0;
    }
    char *end = nullptr;
    const double parsed = std::strtod(raw, &end);
    if (end == raw || parsed <= 0.0)
    {
        return 1.0;
    }
    return parsed;
}

double TimeScale()
{
    static const double kScale = LoadTimeScale();
    return kScale;
}

/// Scale a `std::chrono::milliseconds` deadline by `LOGLIB_TEST_TIME_SCALE`,
/// rounding up so a tight 25 ms budget never collapses to 0 on a 0.5x
/// scale (we never shrink below 1 ms).
std::chrono::milliseconds ScaledMs(std::chrono::milliseconds base)
{
    const double scaled = static_cast<double>(base.count()) * TimeScale();
    const auto rounded = static_cast<long long>(scaled + 0.5);
    return std::chrono::milliseconds(rounded < 1 ? 1 : rounded);
}

/// Test scratch directory + monotonically-named files. Each test case
/// constructs one of these on the stack; the directory tree is removed
/// in the dtor. We pick `temp_directory_path()` over CWD so parallel
/// test runs (e.g. `ctest -j`) cannot collide on a fixed filename.
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

/// Drain the source into a string, parking on `WaitForBytes` between
/// short reads. Returns once `predicate(accumulated)` is true or
/// `deadline` elapses. This is the standard "wait for the worker to
/// produce expected output" helper used throughout the test file.
template <typename Predicate>
std::string DrainUntil(TailingFileSource &source, std::chrono::milliseconds deadline, Predicate predicate)
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

/// Test-default options: no native watcher, short polling cadence so
/// the test wall-clock stays small. The polling interval matches the
/// 250 ms heartbeat the PRD calls out, scaled down 10x for tests.
TailingFileSource::Options FastPollOptions()
{
    TailingFileSource::Options options;
    options.disableNativeWatcher = true;
    options.pollInterval = 25ms;
    options.heartbeatInterval = 25ms;
    options.rotationDebounce = 250ms;
    options.readChunkBytes = 4 * 1024;
    options.prefillChunkBytes = 4 * 1024;
    return options;
}

} // namespace

TEST_CASE("TailingFileSource pre-fill of last N complete lines on a small file", "[TailingFileSource]")
{
    TempDir dir;
    const auto path = dir.File("preset_lines.log");

    std::string content;
    for (int i = 0; i < 100; ++i)
    {
        content += "line " + std::to_string(i) + "\n";
    }
    Overwrite(path, content);

    TailingFileSource source(path, /*retentionLines=*/20, FastPollOptions());

    const std::string drained = DrainUntil(source, ScaledMs(1000ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= 20;
    });

    const auto lines = SplitLines(drained);
    REQUIRE(lines.size() == 20);
    CHECK(lines.front() == "line 80");
    CHECK(lines.back() == "line 99");
    CHECK(source.DisplayName() == path.string());
}

TEST_CASE("TailingFileSource pre-fill on a file shorter than N", "[TailingFileSource]")
{
    TempDir dir;
    const auto path = dir.File("short.log");
    Overwrite(path, "a\nb\nc\n");

    TailingFileSource source(path, /*retentionLines=*/100, FastPollOptions());

    const std::string drained = DrainUntil(source, ScaledMs(500ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= 3;
    });

    const auto lines = SplitLines(drained);
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "a");
    CHECK(lines[1] == "b");
    CHECK(lines[2] == "c");
}

TEST_CASE("TailingFileSource detects growth across many small writes", "[TailingFileSource]")
{
    TempDir dir;
    const auto path = dir.File("growth.log");
    Overwrite(path, ""); // start empty

    TailingFileSource source(path, /*retentionLines=*/10, FastPollOptions());

    constexpr int kCount = 25;
    std::thread writer([&] {
        for (int i = 0; i < kCount; ++i)
        {
            Append(path, "g" + std::to_string(i) + "\n");
            std::this_thread::sleep_for(ScaledMs(5ms));
        }
    });

    const std::string drained = DrainUntil(source, ScaledMs(2000ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= kCount;
    });
    writer.join();

    const auto lines = SplitLines(drained);
    REQUIRE(lines.size() >= static_cast<size_t>(kCount));
    for (int i = 0; i < kCount; ++i)
    {
        CHECK(lines[i] == "g" + std::to_string(i));
    }
}

TEST_CASE("TailingFileSource recovers from rename-and-create rotation", "[TailingFileSource][rotation]")
{
    TempDir dir;
    const auto path = dir.File("rotate.log");
    const auto rotatedPath = dir.File("rotate.log.1");
    Overwrite(path, "pre1\npre2\n");

    std::atomic<int> rotationFires{0};
    TailingFileSource source(path, /*retentionLines=*/100, FastPollOptions());
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

TEST_CASE("TailingFileSource recovers from copytruncate rotation", "[TailingFileSource][rotation]")
{
    TempDir dir;
    const auto path = dir.File("copytrunc.log");
    Overwrite(path, "old1\nold2\nold3\n");

    std::atomic<int> rotationFires{0};
    TailingFileSource source(path, /*retentionLines=*/100, FastPollOptions());
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

TEST_CASE("TailingFileSource recovers from in-place truncate", "[TailingFileSource][rotation]")
{
    TempDir dir;
    const auto path = dir.File("inplace_trunc.log");
    Overwrite(path, "x1\nx2\nx3\n");

    TailingFileSource source(path, /*retentionLines=*/100, FastPollOptions());

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
    "TailingFileSource keeps ingesting while the consumer is paused (rotation included)",
    "[TailingFileSource][rotation]"
)
{
    TempDir dir;
    const auto path = dir.File("paused.log");
    Overwrite(path, "p1\np2\n");

    TailingFileSource source(path, /*retentionLines=*/100, FastPollOptions());

    // The "consumer" never calls Read while we rotate; ingestion (and
    // rotation detection) must still happen on the worker.
    std::this_thread::sleep_for(ScaledMs(50ms)); // let pre-fill settle
    Overwrite(path, "");                         // truncate
    std::this_thread::sleep_for(ScaledMs(50ms));
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

TEST_CASE("TailingFileSource discards partial line on rotation", "[TailingFileSource][rotation]")
{
    TempDir dir;
    const auto path = dir.File("partial_rot.log");
    // Note: no trailing newline → "incomplete" stays in the partial-line buffer.
    Overwrite(path, "complete\nincomplete");

    TailingFileSource source(path, /*retentionLines=*/100, FastPollOptions());

    // Drain "complete" first.
    auto first = DrainUntil(source, ScaledMs(500ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= 1;
    });
    auto firstLines = SplitLines(first);
    REQUIRE(firstLines.size() >= 1);
    CHECK(firstLines[0] == "complete");

    // Rotate via in-place truncate. The partial "incomplete" must be
    // discarded (PRD 4.8.7.i / §7 *Line buffering*).
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

TEST_CASE("TailingFileSource flushes the partial line on Stop", "[TailingFileSource]")
{
    TempDir dir;
    const auto path = dir.File("partial_stop.log");
    Overwrite(path, "first\ntrailing-no-newline");

    TailingFileSource source(path, /*retentionLines=*/100, FastPollOptions());

    auto pre = DrainUntil(source, ScaledMs(500ms), [](const std::string &acc) {
        return std::count(acc.begin(), acc.end(), '\n') >= 1;
    });
    auto preLines = SplitLines(pre);
    REQUIRE(preLines.size() >= 1);
    CHECK(preLines[0] == "first");

    // Now Stop. The partial-line buffer must be flushed as a synthetic
    // last line (PRD 4.7.2.ii).
    source.Stop();

    auto post = DrainUntil(source, ScaledMs(1000ms), [&](const std::string & /*acc*/) { return source.IsClosed(); });
    const auto postLines = SplitLines(post);
    REQUIRE(postLines.size() >= 1);
    CHECK(postLines.back() == "trailing-no-newline");
    CHECK(source.IsClosed());
}

TEST_CASE("TailingFileSource Stop unblocks WaitForBytes parked on an idle file", "[TailingFileSource]")
{
    TempDir dir;
    const auto path = dir.File("idle.log");
    Overwrite(path, ""); // empty; worker will park waiting for growth

    TailingFileSource source(path, /*retentionLines=*/100, FastPollOptions());

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
    // The PRD 8.6 budget is 500 ms for stop teardown including I/O
    // unwind; on this idle source we expect well under that. Scale the
    // budget by `LOGLIB_TEST_TIME_SCALE` for slow CI runners — the PRD
    // budget itself is fixed, but we don't want to flake on a 10x-slow
    // shared-runner CPU.
    CHECK(elapsed < ScaledMs(500ms));
}

TEST_CASE("TailingFileSource debounces rapid rotations within 1 s", "[TailingFileSource][rotation]")
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
    TailingFileSource source(path, /*retentionLines=*/100, opts);
    source.SetRotationCallback([&] { rotationFires.fetch_add(1, std::memory_order_release); });

    // Wait for pre-fill to consume the seed and for the worker to settle
    // on a steady mReadOffset.
    DrainUntil(source, ScaledMs(500ms), [](const std::string &acc) { return acc.find("seed\n") != std::string::npos; });

    // Rename-and-create rotations are detected via branch (i) "file
    // identity changed", which is purely state-based (no timing race
    // against the size-shrunk window). Each rotation flips the path's
    // inode, so the worker observes every one regardless of CI scheduling
    // jitter. We then assert on the *callback fire* count: the debounce
    // collapses all three into one user-visible rotation event (PRD 4.8.9).
    for (int i = 0; i < 3; ++i)
    {
        const auto rotated = dir.File("debounce.log." + std::to_string(i));
        std::filesystem::rename(path, rotated);
        Overwrite(path, "rot" + std::to_string(i) + "\n");
        // Pump twice the poll interval so the worker is guaranteed to
        // observe the new identity before the next rename arrives.
        std::this_thread::sleep_for(ScaledMs(20ms));
    }

    // Drain so we know the worker has processed at least the last rotation.
    DrainUntil(source, ScaledMs(1000ms), [](const std::string &acc) {
        return acc.find("rot2\n") != std::string::npos;
    });

    // The internal RotationCount counts every detected rotation; the
    // external callback fires at most once per debounce window. The
    // callback assertion is the PRD-mandated semantic; the count
    // assertion proves the worker actually observed each event (so the
    // debounce isn't trivially satisfied by missed events).
    CHECK(source.RotationCount() >= 3);
    CHECK(rotationFires.load(std::memory_order_acquire) == 1);
}

TEST_CASE("TailingFileSource throws on initial open failure", "[TailingFileSource]")
{
    TempDir dir;
    const auto path = dir.File("does_not_exist.log");

    CHECK_THROWS_AS(TailingFileSource(path, 100, FastPollOptions()), std::system_error);
}

TEST_CASE("TailingFileSource Stop after natural drain leaves IsClosed true", "[TailingFileSource]")
{
    TempDir dir;
    const auto path = dir.File("simple.log");
    Overwrite(path, "only\n");

    TailingFileSource source(path, /*retentionLines=*/100, FastPollOptions());
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

    // Subsequent Read returns 0 with IsClosed true (PRD 4.9.2.i bullet 3).
    std::array<char, 8> buf{};
    CHECK(source.Read(std::span<char>(buf)) == 0);
    CHECK(source.IsClosed());
}
