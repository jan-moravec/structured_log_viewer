// Session-bundle export / import benchmarks. Uses the same pinned-seed fixture as the parser benchmarks so
// run-over-run numbers are directly comparable. See CONTRIBUTING.md ## Benchmarking for the PR process.

#include "parse_only.hpp"

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"
#include "common.hpp"

#include <loglib/core/log_data.hpp>
#include <loglib/core/log_line.hpp>
#include <loglib/ingest/decompressing_byte_source.hpp>
#include <loglib/output/file_loading.hpp>
#include <loglib/output/session_bundle.hpp>
#include <loglib/parse/parsers/json_parser.hpp>
#include <loglib/schema/log_configuration.hpp>
#include <loglib/table/log_table.hpp>

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>

#include <benchmark/benchmark.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

using namespace loglib;

namespace bench
{

namespace
{

// 250'000 lines (about 43 MiB of JSONL). The write benchmark takes about a second per run, so a benchmark with its
// warm-up and five repetitions stays within the time budget.
constexpr std::size_t BUNDLE_RECORDS = 250'000;

/**
 * RAII bundle path that also removes unique staging siblings.
 */
class TempBundlePath
{
public:
    explicit TempBundlePath(std::string_view extension = ".slvbundle")
    {
        // Atomic counter is drift-proofing for future parallel runs;
        // benchmarks are sequential today.
        static std::atomic<int> counter{0};
        const auto tmpDir = std::filesystem::temp_directory_path();
        while (true)
        {
            const int n = ++counter;
            mPath = tmpDir / (std::string("slv_bench_bundle_") + std::to_string(n) + std::string(extension));
            if (!std::filesystem::exists(mPath))
            {
                break;
            }
        }
    }

    ~TempBundlePath()
    {
        try
        {
            std::error_code ec;
            std::filesystem::remove(mPath, ec);
            // Sweep randomized staging names using native path strings.
            const auto parent = mPath.parent_path();
            std::error_code iterEc;
            if (!std::filesystem::exists(parent, iterEc))
            {
                return;
            }
            const auto basename = mPath.filename().native();
            const std::filesystem::path::string_type tmpSuffix = std::filesystem::path(".tmp").native();
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
        catch (...) // NOLINT(bugprone-empty-catch): destructor must not throw during benchmark teardown.
        {
        }
    }

    TempBundlePath(const TempBundlePath &) = delete;
    TempBundlePath &operator=(const TempBundlePath &) = delete;
    TempBundlePath(TempBundlePath &&) = delete;
    TempBundlePath &operator=(TempBundlePath &&) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const noexcept
    {
        return mPath;
    }

private:
    std::filesystem::path mPath;
};

// A large LogTable built from a JSON fixture with the same seed, timestamps and record shape as the parser
// benchmarks. The fixture file stays alive because the table reads from it. Requires timezone init for the timestamp
// columns.
struct TableFixture
{
    std::unique_ptr<TestStructuredLogFile> file;
    std::unique_ptr<LoadResult> loaded;
    std::size_t bytes = 0;
    std::size_t recordCount = 0;

    LogTable &Table() const
    {
        return loaded->table;
    }
};

std::unique_ptr<TableFixture> BuildLargeJsonTable()
{
    auto fixture = std::make_unique<TableFixture>();
    fixture->file = std::make_unique<TestStructuredLogFile>(
        StreamedRecords{
            .count = BUNDLE_RECORDS,
            .seed = LARGE_FIXTURE_SEED,
            .timestamps = DeterministicBenchmarkTimestamps(),
        },
        test_common::JsonLines(),
        test_common::RecordSchema{},
        "bench_session_bundle.jsonl"
    );
    fixture->bytes = std::filesystem::file_size(fixture->file->GetFilePath());
    fixture->recordCount = fixture->file->RecordCount();

    InitializeTimezoneData();

    fixture->loaded = std::make_unique<LoadResult>(LoadFile(fixture->file->GetFilePath()));
    if (!fixture->loaded->errors.empty() || fixture->loaded->table.RowCount() != fixture->recordCount)
    {
        throw std::runtime_error("the bundle fixture did not load");
    }
    return fixture;
}

using SharedTable = std::shared_ptr<LazyInput<TableFixture>>;

struct BundleInput
{
    SharedTable table;
    // Encoded once at level 3 (writer default) so the read benchmark measures pure decode + rehydrate cost.
    std::unique_ptr<TempBundlePath> bundlePath;
    std::size_t compressedBytes = 0;
};

struct BundleTextInput
{
    SharedTable table;
    // The rows of a bundle, decoded once to a plain text file.
    std::unique_ptr<TempBundlePath> textPath;
};

// Level 3 with the given number of zstd workers. A negative number compresses on the calling thread.
SessionBundleWriteOptions WriteOptions(int workers = -1)
{
    SessionBundleWriteOptions opts;
    opts.compressionLevel = 3;
    opts.totalWorkers = workers;
    return opts;
}

// Decodes a bundle and parses its rows; true when the metadata and the parse agree with @p recordCount.
bool DecodeAndParse(const std::filesystem::path &bundle, std::size_t recordCount)
{
    DecompressingByteSource::Options options;
    options.discardFirstLine = true;
    const DecompressingByteSource decoded(bundle, {}, {}, options);
    const SessionBundleMetadata metadata = ParseSessionBundleMetadata(decoded.DiscardedFirstLine());
    if (metadata.rowCount != recordCount)
    {
        return false;
    }
    const ParseResult parsed = ParseFile(decoded.EffectivePath());
    return parsed.errors.empty() && parsed.data.Lines().size() == recordCount;
}

void SetBundleCounters(benchmark::State &state, const TableFixture &fixture)
{
    // MB/s over the *uncompressed* input size, matching the parse benchmarks, so ratios show the codec speed relative
    // to raw log volume.
    const auto iterations = static_cast<std::int64_t>(state.iterations());
    state.SetBytesProcessed(iterations * static_cast<std::int64_t>(fixture.bytes));
    state.SetItemsProcessed(iterations * static_cast<std::int64_t>(fixture.recordCount));
}

struct TableOnly
{
    SharedTable table;
};

} // namespace

void RegisterSessionBundleBenchmarks()
{
    const SharedTable table = MakeLazy<TableFixture>(BuildLargeJsonTable);

    // Bundle-write throughput. Level 3 is the writer's default. The default number of workers is left to the writer.
    for (const auto &[name, workers] : {
             std::pair<const char *, int>{"WriteSessionBundle 250'000 JSON entries (level=3, single-threaded)", -1},
             std::pair<const char *, int>{"WriteSessionBundle 250'000 JSON entries (level=3, default workers)", 0},
         })
    {
        RegisterLarge<TableOnly>(
            name,
            [table] { return std::make_unique<TableOnly>(TableOnly{.table = table}); },
            [workers](TableOnly &input, benchmark::State &state, bool first) {
                const TableFixture &fixture = input.table->Get();
                const LogConfiguration &cfg = fixture.Table().Configuration().Configuration();
                std::size_t compressed = 0;
                const auto writeOnce = [&] {
                    const TempBundlePath dest;
                    WriteSessionBundle(fixture.Table(), cfg, dest.Path(), WriteOptions(workers));
                    compressed = std::filesystem::file_size(dest.Path());
                };
                if (first)
                {
                    writeOnce();
                }
                for (const auto _ : state)
                {
                    writeOnce();
                }
                SetBundleCounters(state, fixture);
                state.counters["bundle_MiB"] = static_cast<double>(compressed) / (1024.0 * 1024.0);
                state.counters["compression_ratio"] =
                    static_cast<double>(fixture.bytes) / static_cast<double>(compressed == 0 ? 1 : compressed);
            }
        );
    }

    // Bundle-read throughput.
    RegisterLarge<BundleInput>(
        "Decompress and parse session bundle (250'000 JSON entries)",
        [table] {
            auto input = std::make_unique<BundleInput>();
            input->table = table;
            const TableFixture &fixture = table->Get();
            input->bundlePath = std::make_unique<TempBundlePath>();
            WriteSessionBundle(
                fixture.Table(),
                fixture.Table().Configuration().Configuration(),
                input->bundlePath->Path(),
                WriteOptions()
            );
            input->compressedBytes = std::filesystem::file_size(input->bundlePath->Path());
            return input;
        },
        [](BundleInput &input, benchmark::State &state, bool first) {
            const TableFixture &fixture = input.table->Get();
            bool ok = !first || DecodeAndParse(input.bundlePath->Path(), fixture.recordCount);
            for (const auto _ : state)
            {
                ok = DecodeAndParse(input.bundlePath->Path(), fixture.recordCount);
            }
            if (!ok)
            {
                Fail(state, "the bundle did not decode to the written rows");
                return;
            }
            SetBundleCounters(state, fixture);
            state.counters["bundle_MiB"] = static_cast<double>(input.compressedBytes) / (1024.0 * 1024.0);
        },
        LargeOptions{.iterations = FAST_LARGE_ITERATIONS}
    );

    // The rows a bundle holds, decoded once in setup and parsed as plain text: the reference the bundle read is
    // compared with.
    RegisterLarge<BundleTextInput>(
        "Parse text of the bundle rows (250'000 JSON entries)",
        [table] {
            auto input = std::make_unique<BundleTextInput>();
            input->table = table;
            const TableFixture &fixture = table->Get();
            const TempBundlePath bundlePath;
            WriteSessionBundle(
                fixture.Table(), fixture.Table().Configuration().Configuration(), bundlePath.Path(), WriteOptions()
            );
            DecompressingByteSource::Options options;
            options.discardFirstLine = true;
            const DecompressingByteSource decoded(bundlePath.Path(), {}, {}, options);
            input->textPath = std::make_unique<TempBundlePath>(".jsonl");
            std::filesystem::copy_file(decoded.EffectivePath(), input->textPath->Path());
            return input;
        },
        [](BundleTextInput &input, benchmark::State &state, bool first) {
            const TableFixture &fixture = input.table->Get();
            const auto parseOnce = [&] {
                const ParseResult parsed = ParseFile(input.textPath->Path());
                return parsed.errors.empty() && parsed.data.Lines().size() == fixture.recordCount;
            };
            bool ok = !first || parseOnce();
            for (const auto _ : state)
            {
                ok = parseOnce();
            }
            if (!ok)
            {
                Fail(state, "the text did not parse to the written rows");
                return;
            }
            SetBundleCounters(state, fixture);
        },
        LargeOptions{.iterations = FAST_LARGE_ITERATIONS}
    );

    // End-to-end round-trip: encode + decode back-to-back. Approximates the "share and re-open" user story so the
    // aggregate wall time is actionable.
    RegisterLarge<TableOnly>(
        "Round-trip 250'000 JSON entries (write + read)",
        [table] { return std::make_unique<TableOnly>(TableOnly{.table = table}); },
        [](TableOnly &input, benchmark::State &state, bool first) {
            const TableFixture &fixture = input.table->Get();
            const LogConfiguration &cfg = fixture.Table().Configuration().Configuration();
            const auto roundTrip = [&] {
                const TempBundlePath dest;
                WriteSessionBundle(fixture.Table(), cfg, dest.Path(), WriteOptions());
                return DecodeAndParse(dest.Path(), fixture.recordCount);
            };
            bool ok = !first || roundTrip();
            for (const auto _ : state)
            {
                ok = roundTrip();
            }
            if (!ok)
            {
                Fail(state, "the round trip changed the rows");
                return;
            }
            SetBundleCounters(state, fixture);
        }
    );
}

} // namespace bench
