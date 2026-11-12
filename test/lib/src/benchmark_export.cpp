// Export benchmarks: JSON Lines, CSV, Markdown and snapshot export of every row, JSON Lines export of rows in a
// shuffled order, and the session bundle write without compression. The exports go to a sink that only counts, so the
// figures measure the serialization and not the disk. The bundle write goes to a temporary file, so its figure includes
// writing and flushing that file. All of them report bytes per second of the output. See CONTRIBUTING.md ##
// Benchmarking.

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"
#include "row_memory.hpp"

#include <loglib/core/stop_token.hpp>
#include <loglib/output/exports/export_sink.hpp>
#include <loglib/output/exports/row_exporter.hpp>
#include <loglib/output/session_bundle.hpp>
#include <loglib/table/log_table.hpp>

#include <benchmark/benchmark.h>
#include <zstd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace loglib;
using loglib::exports::ExportFormat;
using loglib::exports::ExportSink;
using loglib::exports::MakeExporter;
using loglib::exports::RowSource;

namespace bench
{

namespace
{

// Counts the bytes and the line breaks of an export and keeps nothing.
class CountingSink final : public ExportSink
{
public:
    void Write(std::string_view bytes) override
    {
        mBytes += bytes.size();
        mLines += static_cast<std::size_t>(std::ranges::count(bytes, '\n'));
    }
    void Finish() override
    {
    }
    [[nodiscard]] std::size_t Bytes() const noexcept
    {
        return mBytes;
    }
    [[nodiscard]] std::size_t Lines() const noexcept
    {
        return mLines;
    }

private:
    std::size_t mBytes = 0;
    std::size_t mLines = 0;
};

// A loaded table with every row and every column selected for export.
struct ExportTable
{
    GeneratedJsonTable generated;
    std::vector<std::size_t> rows;
    std::vector<std::size_t> shuffledRows;
    std::vector<std::size_t> columns;

    [[nodiscard]] const LogTable &Table() const
    {
        return generated.loaded->table;
    }
};

using SharedExportTable = std::shared_ptr<LazyInput<ExportTable>>;

SharedExportTable MakeSharedExportTable(const Tier &tier)
{
    return MakeLazy<ExportTable>([rows = tier.rows, smoke = tier.smoke] {
        auto input = std::make_unique<ExportTable>();
        input->generated = LoadGeneratedJsonTable(rows, smoke ? "bench_export_smoke.jsonl" : "bench_export.jsonl");
        const LogTable &table = input->Table();
        input->rows.resize(table.RowCount());
        std::iota(input->rows.begin(), input->rows.end(), std::size_t{0});
        input->shuffledRows = input->rows;
        // A fixed seed keeps the run repeatable.
        // NOLINTNEXTLINE(bugprone-random-generator-seed,cert-msc32-c,cert-msc51-cpp)
        std::mt19937_64 generator(42);
        std::ranges::shuffle(input->shuffledRows, generator);
        input->columns.resize(table.ColumnCount());
        std::iota(input->columns.begin(), input->columns.end(), std::size_t{0});
        return input;
    });
}

struct ExportInput
{
    SharedExportTable table;
};

// One export of every row, in table order or in a fixed random order. The header of a CSV adds one line and the header
// and separator of a Markdown table add two.
void RegisterExport(
    const Tier &tier, const SharedExportTable &table, const char *label, ExportFormat format, bool shuffled = false
)
{
    std::size_t headerLines = 0;
    if (format == ExportFormat::Csv)
    {
        headerLines = 1;
    }
    else if (format == ExportFormat::Markdown)
    {
        headerLines = 2;
    }
    RegisterLarge<ExportInput>(
        TierName(std::string(label) + " export of " + CountText(tier.rows) + " rows", tier),
        [table] { return std::make_unique<ExportInput>(ExportInput{.table = table}); },
        [format, headerLines, shuffled, rows = tier.rows](ExportInput &input, benchmark::State &state, bool first) {
            const ExportTable &fixture = input.table->Get();
            const RowSource source{
                .table = &fixture.Table(),
                .sourceRows = shuffled ? fixture.shuffledRows : fixture.rows,
                .visibleColumns = fixture.columns,
                .includeAllFieldsForJson = true,
                .includeHeaderRow = true,
            };
            const auto exporter = MakeExporter(format);
            std::size_t bytes = 0;
            std::size_t lines = 0;
            const auto exportOnce = [&] {
                CountingSink sink;
                exporter->Run(source, sink, StopToken{});
                bytes = sink.Bytes();
                lines = sink.Lines();
            };
            if (first)
            {
                exportOnce();
            }
            for (const auto _ : state)
            {
                exportOnce();
            }
            if (lines != rows + headerLines || bytes == 0)
            {
                Fail(state, "the export wrote " + std::to_string(lines) + " lines, expected " + std::to_string(rows));
                return;
            }
            const auto iterations = static_cast<std::int64_t>(state.iterations());
            state.SetBytesProcessed(iterations * static_cast<std::int64_t>(bytes));
            state.SetItemsProcessed(iterations * static_cast<std::int64_t>(rows));
            state.counters["output_MiB"] = static_cast<double>(bytes) / (1024.0 * 1024.0);
        },
        LargeOptions{.repetitions = tier.largeRepetitions}
    );
}

// A path in the temporary folder that is removed when it goes out of scope.
class TempFile
{
public:
    TempFile()
    {
        static std::atomic<int> counter{0};
        mPath =
            std::filesystem::temp_directory_path() / ("slv_bench_export_" + std::to_string(++counter) + ".slvbundle");
    }
    ~TempFile()
    {
        std::error_code ec;
        std::filesystem::remove(mPath, ec);
    }
    TempFile(const TempFile &) = delete;
    TempFile &operator=(const TempFile &) = delete;
    TempFile(TempFile &&) = delete;
    TempFile &operator=(TempFile &&) = delete;

    [[nodiscard]] const std::filesystem::path &Path() const noexcept
    {
        return mPath;
    }

private:
    std::filesystem::path mPath;
};

// The session bundle write at the lowest zstd level, which stores almost every block as it is, so the figure is the
// serialization and the framing and not the compressor.
void RegisterBundleWrite(const Tier &tier, const SharedExportTable &table)
{
    RegisterLarge<ExportInput>(
        TierName("WriteSessionBundle " + CountText(tier.rows) + " JSON entries (no compression)", tier),
        [table] { return std::make_unique<ExportInput>(ExportInput{.table = table}); },
        [rows = tier.rows](ExportInput &input, benchmark::State &state, bool first) {
            const ExportTable &fixture = input.table->Get();
            SessionBundleWriteOptions options;
            options.compressionLevel = ZSTD_minCLevel();
            options.totalWorkers = -1;
            std::uintmax_t bytes = 0;
            const auto writeOnce = [&] {
                const TempFile destination;
                WriteSessionBundle(
                    fixture.Table(), fixture.Table().Configuration().Configuration(), destination.Path(), options
                );
                bytes = std::filesystem::file_size(destination.Path());
            };
            if (first)
            {
                writeOnce();
            }
            for (const auto _ : state)
            {
                writeOnce();
            }
            if (bytes == 0)
            {
                Fail(state, "the bundle is empty");
                return;
            }
            const auto iterations = static_cast<std::int64_t>(state.iterations());
            state.SetBytesProcessed(iterations * static_cast<std::int64_t>(bytes));
            state.SetItemsProcessed(iterations * static_cast<std::int64_t>(rows));
            state.counters["bundle_MiB"] = static_cast<double>(bytes) / (1024.0 * 1024.0);
        },
        LargeOptions{.repetitions = tier.largeRepetitions}
    );
}

} // namespace

void RegisterExportBenchmarks()
{
    // An export takes about 5 microseconds a row, so the full tier uses a quarter of the rows of the other tables to
    // stay within the time of one benchmark. The smoke tier has the JSON Lines export.
    constexpr Tier EXPORT_TIER{.rows = 250'000};
    const SharedExportTable full = MakeSharedExportTable(EXPORT_TIER);
    RegisterExport(EXPORT_TIER, full, "JSON Lines", ExportFormat::JsonLines);
    RegisterExport(EXPORT_TIER, full, "CSV", ExportFormat::Csv);
    RegisterExport(EXPORT_TIER, full, "Markdown", ExportFormat::Markdown);
    RegisterExport(EXPORT_TIER, full, "Snapshot", ExportFormat::Snapshot);
    RegisterExport(EXPORT_TIER, full, "JSON Lines (shuffled rows)", ExportFormat::JsonLines, true);
    RegisterBundleWrite(EXPORT_TIER, full);

    const SharedExportTable smoke = MakeSharedExportTable(SMOKE_TIER);
    RegisterExport(SMOKE_TIER, smoke, "JSON Lines", ExportFormat::JsonLines);
}

} // namespace bench
