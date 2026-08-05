// Session-bundle export / import benchmarks. Uses the same
// `[large]`-style pinned RNG seeded fixture as the parser benchmarks
// so run-over-run numbers are directly comparable. See CONTRIBUTING.md
// `## Benchmarking` for the PR process.

#include "benchmark_common.hpp"
#include "common.hpp"

#include <loglib/internal/decompressing_byte_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_table.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/session_bundle.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>

#include <catch2/catch_all.hpp>

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <system_error>
#include <utility>
#include <vector>

using namespace loglib;
using namespace bench;

namespace
{

/// RAII-owned bundle destination path. Unlinks the file plus any
/// staging siblings the writer might have left behind on scope
/// exit -- the writer picks a randomised `<path>.<seed>.<counter>.tmp`
/// suffix per call, so a benchmark that aborts mid-write can leave
/// files that a fixed `<path>.tmp` sweep would miss.
class TempBundlePath
{
public:
    TempBundlePath()
    {
        // Atomic counter is drift-proofing for future parallel runs;
        // benchmarks are sequential today.
        static std::atomic<int> counter{0};
        const auto tmpDir = std::filesystem::temp_directory_path();
        do
        {
            const int n = ++counter;
            mPath = tmpDir / (std::string("slv_bench_bundle_") + std::to_string(n) + ".slvbundle");
        } while (std::filesystem::exists(mPath));
    }

    ~TempBundlePath()
    {
        std::error_code ec;
        std::filesystem::remove(mPath, ec);
        // Sweep every `<basename>*.tmp` sibling; the writer's suffix
        // is randomised per invocation so we cannot know the exact
        // name up front. Comparing on `native()` avoids the UTF-8
        // vs. wide re-encode on Windows.
        const auto parent = mPath.parent_path();
        std::error_code iterEc;
        if (!std::filesystem::exists(parent, iterEc))
        {
            return;
        }
        const auto basename = mPath.filename().native();
        const std::filesystem::path::string_type tmpSuffix =
            std::filesystem::path(".tmp").native();
        for (const auto &entry : std::filesystem::directory_iterator(parent, iterEc))
        {
            const auto name = entry.path().filename().native();
            if (name == basename)
            {
                continue;
            }
            if (name.starts_with(basename) && name.ends_with(tmpSuffix))
            {
                std::error_code rmEc;
                std::filesystem::remove(entry.path(), rmEc);
            }
        }
    }

    TempBundlePath(const TempBundlePath &) = delete;
    TempBundlePath &operator=(const TempBundlePath &) = delete;
    TempBundlePath(TempBundlePath &&) = delete;
    TempBundlePath &operator=(TempBundlePath &&) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const noexcept { return mPath; }

private:
    std::filesystem::path mPath;
};

/// Build a large `LogTable` from a JSON fixture identical (bytes) to
/// the `[large]` parser benchmarks -- same seed, timestamps, and
/// record shape. Requires timezone init for the timestamp columns.
loglib::LogTable BuildLargeJsonTable(std::size_t &outBytes, std::size_t &outRecordCount)
{
    const TestStructuredLogFile testFile(
        StreamedRecords{
            .count = 1'000'000, .seed = LARGE_FIXTURE_SEED, .timestamps = DeterministicBenchmarkTimestamps()
        },
        test_common::JsonLines()
    );
    outBytes = std::filesystem::file_size(testFile.GetFilePath());
    outRecordCount = testFile.RecordCount();

    InitializeTimezoneData();

    JsonParser parser;
    ParseResult result = ParseFile(parser, testFile.GetFilePath());
    REQUIRE(result.errors.empty());
    REQUIRE(result.data.Lines().size() == outRecordCount);

    LogTable table;
    table.Update(std::move(result.data));
    REQUIRE(table.RowCount() == outRecordCount);
    return table;
}

} // namespace

// Bundle-write throughput: mirrors `[json_parser][large]` in fixture
// size + seed so numbers are directly comparable to the parse path.
// Level `3` is the writer's default; the harness reports MB/s over the
// uncompressed input size so it stays comparable to the parse benchmarks.
TEST_CASE("Write session bundle (JSON, 1'000'000 lines)", "[.][benchmark][session_bundle][write]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    std::size_t bytes = 0;
    std::size_t recordCount = 0;
    LogTable table = BuildLargeJsonTable(bytes, recordCount);

    const LogConfiguration &cfg = table.Configuration().Configuration();

    RunTimedSamples(
        "WriteSessionBundle 1'000'000 JSON entries (level=3, single-threaded)", 3,
        {.bytes = bytes, .lines = recordCount},
        [&]() {
            const TempBundlePath dest;
            SessionBundleWriteOptions opts;
            opts.compressionLevel = 3;
            opts.totalWorkers = 0;
            WriteSessionBundle(table, cfg, dest.Path(), opts);
            const std::size_t compressed = std::filesystem::file_size(dest.Path());
            WARN(
                "Bundle size: " << (static_cast<double>(compressed) / (1024.0 * 1024.0)) << " MiB (ratio "
                                << (static_cast<double>(bytes) / static_cast<double>(compressed == 0 ? 1 : compressed))
                                << "x)"
            );
        }
    );
}

// Bundle-read throughput. Report MB/s over the *uncompressed* input
// size (matches the write benchmark) so ratios show the codec's decode
// speed relative to raw log volume.
TEST_CASE("Read session bundle (JSON, 1'000'000 lines)", "[.][benchmark][session_bundle][read]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    std::size_t bytes = 0;
    std::size_t recordCount = 0;
    LogTable table = BuildLargeJsonTable(bytes, recordCount);

    const LogConfiguration &cfg = table.Configuration().Configuration();

    // Encode once at level 3 (writer default) so the read benchmark
    // measures pure decode + rehydrate cost, not encode variance.
    const TempBundlePath bundlePath;
    {
        SessionBundleWriteOptions opts;
        opts.compressionLevel = 3;
        opts.totalWorkers = 0;
        WriteSessionBundle(table, cfg, bundlePath.Path(), opts);
    }
    const std::size_t compressed = std::filesystem::file_size(bundlePath.Path());
    WARN(
        "Bundle on-disk size: " << (static_cast<double>(compressed) / (1024.0 * 1024.0)) << " MiB (compressed) vs "
                                << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MiB (uncompressed)"
    );

    RunTimedSamples(
        "Decompress and parse session bundle (1'000'000 JSON entries)", 3,
        {.bytes = bytes, .lines = recordCount},
        [&]() {
            internal::DecompressingByteSource::Options options;
            options.discardFirstLine = true;
            internal::DecompressingByteSource decoded(bundlePath.Path(), {}, {}, options);
            const SessionBundleMetadata metadata = ParseSessionBundleMetadata(decoded.DiscardedFirstLine());
            REQUIRE(metadata.rowCount == recordCount);
            const ParseResult parsed = ParseFile(decoded.EffectivePath());
            REQUIRE(parsed.errors.empty());
            REQUIRE(parsed.data.Lines().size() == recordCount);
        }
    );
}

// End-to-end round-trip: encode + decode back-to-back. Approximates
// the "share and re-open" user story so the aggregate wall time is
// actionable (e.g. "sharing a 1M-line session takes <N> seconds").
TEST_CASE("Round-trip session bundle (JSON, 1'000'000 lines)", "[.][benchmark][session_bundle][round_trip]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    std::size_t bytes = 0;
    std::size_t recordCount = 0;
    LogTable table = BuildLargeJsonTable(bytes, recordCount);

    const LogConfiguration &cfg = table.Configuration().Configuration();

    RunTimedSamples(
        "Round-trip 1'000'000 JSON entries (write + read)", 3, {.bytes = bytes, .lines = recordCount},
        [&]() {
            const TempBundlePath dest;
            SessionBundleWriteOptions writeOpts;
            writeOpts.compressionLevel = 3;
            writeOpts.totalWorkers = 0;
            WriteSessionBundle(table, cfg, dest.Path(), writeOpts);

            internal::DecompressingByteSource::Options options;
            options.discardFirstLine = true;
            internal::DecompressingByteSource decoded(dest.Path(), {}, {}, options);
            const SessionBundleMetadata metadata = ParseSessionBundleMetadata(decoded.DiscardedFirstLine());
            REQUIRE(metadata.rowCount == recordCount);
            const ParseResult parsed = ParseFile(decoded.EffectivePath());
            REQUIRE(parsed.errors.empty());
            REQUIRE(parsed.data.Lines().size() == recordCount);
        }
    );
}
