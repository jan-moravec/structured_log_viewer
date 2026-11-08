// Decompression benchmark for DecompressingByteSource. Measures end-to-end DecompressingByteSource + ParseFile
// on a JSONL fixture per codec against the uncompressed baseline. The figures are MB/s of the file that is read: the
// compressed size for a codec, the plain size for the baseline. See CONTRIBUTING.md ## Benchmarking.

#include "parse_only.hpp"

#include "benchmark_common.hpp"
#include "benchmark_registry.hpp"
#include "common.hpp"

#include <loglib/ingest/decompressing_byte_source.hpp>
#include <loglib/output/file_loading.hpp>
#include <loglib/parse/parsers/json_parser.hpp>

#include <test_common/log_format.hpp>

#include <benchmark/benchmark.h>

#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using loglib::DecompressingByteSource;
using loglib::JsonParser;
using loglib::LoadFile;
using loglib::LoadResult;
using loglib::ParseFile;

namespace bench
{

namespace
{

// 250'000 lines, about 43 MiB of JSONL. Large enough for the multi-threaded xz decoder to see several blocks, small
// enough that the slowest codec (bzip2) stays within the time budget of a benchmark.
constexpr std::size_t BENCH_LINE_COUNT = 250'000;
constexpr std::size_t IO_CHUNK = 256 * 1024;

// The fixture writers throw instead of asserting: the benchmark executable has no test session.
void Check(bool ok)
{
    if (!ok)
    {
        throw std::runtime_error("could not build the compressed fixture");
    }
}

/**
 * Delete @p path (best-effort) when the RAII guard goes out of scope.
 */
class FileScrubGuard
{
public:
    explicit FileScrubGuard(std::filesystem::path path) noexcept
        : mPath(std::move(path))
    {
    }
    ~FileScrubGuard()
    {
        std::error_code ec;
        std::filesystem::remove(mPath, ec);
    }
    FileScrubGuard(const FileScrubGuard &) = delete;
    FileScrubGuard &operator=(const FileScrubGuard &) = delete;
    FileScrubGuard(FileScrubGuard &&) = delete;
    FileScrubGuard &operator=(FileScrubGuard &&) = delete;

private:
    std::filesystem::path mPath;
};

std::string BenchScratchPath(const std::string &suffix)
{
    static const std::filesystem::path SCRATCH_ROOT = [] {
        auto p = std::filesystem::temp_directory_path() / "slv-bench-decompression";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p;
    }();
    return (SCRATCH_ROOT / ("decompression_fixture" + suffix)).string();
}

// -------- streaming compressors (input file -> compressed output file) --------

void CompressToGzip(const std::filesystem::path &input, const std::filesystem::path &output)
{
    std::ifstream in(input, std::ios::binary);
    std::ofstream out(output, std::ios::binary);
    Check((in.is_open() && out.is_open()));

    z_stream strm{};
    Check(::deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);

    std::array<Bytef, IO_CHUNK> inBuf{};
    std::array<Bytef, IO_CHUNK> outBuf{};
    int flush = Z_NO_FLUSH;
    while (flush != Z_FINISH)
    {
        in.read(reinterpret_cast<char *>(inBuf.data()), static_cast<std::streamsize>(inBuf.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        flush = in.eof() ? Z_FINISH : Z_NO_FLUSH;
        strm.next_in = inBuf.data();
        strm.avail_in = static_cast<uInt>(got);

        while (true)
        {
            strm.next_out = outBuf.data();
            strm.avail_out = static_cast<uInt>(outBuf.size());
            const int ret = ::deflate(&strm, flush);
            Check(ret != Z_STREAM_ERROR);
            const std::size_t produced = outBuf.size() - strm.avail_out;
            out.write(reinterpret_cast<const char *>(outBuf.data()), static_cast<std::streamsize>(produced));
            if (strm.avail_out != 0)
            {
                break;
            }
        }
    }
    ::deflateEnd(&strm);
}

void CompressToBzip2(const std::filesystem::path &input, const std::filesystem::path &output)
{
    std::ifstream in(input, std::ios::binary);
    std::ofstream out(output, std::ios::binary);
    Check((in.is_open() && out.is_open()));

    bz_stream strm{};
    Check(::BZ2_bzCompressInit(&strm, 9, 0, 30) == BZ_OK);

    std::array<char, IO_CHUNK> inBuf{};
    std::array<char, IO_CHUNK> outBuf{};
    int action = BZ_RUN;
    // Loop until BZ_STREAM_END. Do NOT exit on avail_in == 0 in
    // FINISH mode -- bzip2 returns BZ_FINISH_OK (not BZ_STREAM_END)
    // while flushing internal buffers, and the final flush can
    // exceed a single 256 KiB output buffer.
    while (true)
    {
        if (action != BZ_FINISH && strm.avail_in == 0)
        {
            in.read(inBuf.data(), static_cast<std::streamsize>(inBuf.size()));
            const auto got = static_cast<std::size_t>(in.gcount());
            strm.next_in = inBuf.data();
            strm.avail_in = static_cast<unsigned>(got);
            if (in.eof())
            {
                action = BZ_FINISH;
            }
        }
        strm.next_out = outBuf.data();
        strm.avail_out = static_cast<unsigned>(outBuf.size());
        const int ret = ::BZ2_bzCompress(&strm, action);
        Check((ret == BZ_RUN_OK || ret == BZ_FINISH_OK || ret == BZ_STREAM_END));
        const std::size_t produced = outBuf.size() - strm.avail_out;
        out.write(outBuf.data(), static_cast<std::streamsize>(produced));
        if (ret == BZ_STREAM_END)
        {
            break;
        }
    }
    ::BZ2_bzCompressEnd(&strm);
}

void CompressToXz(const std::filesystem::path &input, const std::filesystem::path &output)
{
    std::ifstream in(input, std::ios::binary);
    std::ofstream out(output, std::ios::binary);
    Check((in.is_open() && out.is_open()));

    lzma_stream strm = LZMA_STREAM_INIT;
    // Preset 1 keeps 500 MiB compression time reasonable.
    Check(::lzma_easy_encoder(&strm, 1, LZMA_CHECK_CRC64) == LZMA_OK);

    std::array<std::uint8_t, IO_CHUNK> inBuf{};
    std::array<std::uint8_t, IO_CHUNK> outBuf{};
    lzma_action action = LZMA_RUN;
    lzma_ret ret = LZMA_OK;
    while (ret != LZMA_STREAM_END)
    {
        if (strm.avail_in == 0 && action == LZMA_RUN)
        {
            in.read(reinterpret_cast<char *>(inBuf.data()), static_cast<std::streamsize>(inBuf.size()));
            const auto got = static_cast<std::size_t>(in.gcount());
            strm.next_in = inBuf.data();
            strm.avail_in = got;
            if (in.eof())
            {
                action = LZMA_FINISH;
            }
        }
        strm.next_out = outBuf.data();
        strm.avail_out = outBuf.size();
        ret = ::lzma_code(&strm, action);
        Check((ret == LZMA_OK || ret == LZMA_STREAM_END));
        const std::size_t produced = outBuf.size() - strm.avail_out;
        out.write(reinterpret_cast<const char *>(outBuf.data()), static_cast<std::streamsize>(produced));
    }
    ::lzma_end(&strm);
}

// Multi-block .xz fixture (equivalent to xz -T <N>).
// lzma_stream_encoder_mt emits per-block headers with populated
// compressed-size fields -- both preconditions for the MT decoder.
// The single-block CompressToXz fixture is invisible to the MT
// decoder (it falls back to ST), so this variant is what actually
// exercises the MT decoder path.
void CompressToXzMt(const std::filesystem::path &input, const std::filesystem::path &output)
{
    std::ifstream in(input, std::ios::binary);
    std::ofstream out(output, std::ios::binary);
    Check((in.is_open() && out.is_open()));

    lzma_stream strm = LZMA_STREAM_INIT;
    std::uint32_t threads = ::lzma_cputhreads();
    if (threads == 0)
    {
        threads = 2; // >=2 blocks so the MT decoder has work to split
    }
    lzma_mt mt = {};
    mt.flags = 0;
    mt.threads = threads;
    mt.timeout = 0;
    mt.preset = 1; // match CompressToXz so fixture sizes stay comparable
    mt.filters = nullptr;
    mt.check = LZMA_CHECK_CRC64;
    Check(::lzma_stream_encoder_mt(&strm, &mt) == LZMA_OK);

    std::array<std::uint8_t, IO_CHUNK> inBuf{};
    std::array<std::uint8_t, IO_CHUNK> outBuf{};
    lzma_action action = LZMA_RUN;
    lzma_ret ret = LZMA_OK;
    while (ret != LZMA_STREAM_END)
    {
        if (strm.avail_in == 0 && action == LZMA_RUN)
        {
            in.read(reinterpret_cast<char *>(inBuf.data()), static_cast<std::streamsize>(inBuf.size()));
            const auto got = static_cast<std::size_t>(in.gcount());
            strm.next_in = inBuf.data();
            strm.avail_in = got;
            if (in.eof())
            {
                action = LZMA_FINISH;
            }
        }
        strm.next_out = outBuf.data();
        strm.avail_out = outBuf.size();
        ret = ::lzma_code(&strm, action);
        Check((ret == LZMA_OK || ret == LZMA_STREAM_END));
        const std::size_t produced = outBuf.size() - strm.avail_out;
        out.write(reinterpret_cast<const char *>(outBuf.data()), static_cast<std::streamsize>(produced));
    }
    ::lzma_end(&strm);
}

void CompressToZstd(const std::filesystem::path &input, const std::filesystem::path &output)
{
    std::ifstream in(input, std::ios::binary);
    std::ofstream out(output, std::ios::binary);
    Check((in.is_open() && out.is_open()));

    ZSTD_CCtx *cctx = ::ZSTD_createCCtx();
    Check(cctx != nullptr);

    const std::size_t inChunk = ::ZSTD_CStreamInSize();
    const std::size_t outChunk = ::ZSTD_CStreamOutSize();
    std::vector<char> inBuf(inChunk);
    std::vector<char> outBuf(outChunk);

    bool finished = false;
    while (!finished)
    {
        in.read(inBuf.data(), static_cast<std::streamsize>(inBuf.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        const ZSTD_EndDirective mode = in.eof() ? ZSTD_e_end : ZSTD_e_continue;
        ZSTD_inBuffer inChunkView{.src = inBuf.data(), .size = got, .pos = 0};
        while (inChunkView.pos < inChunkView.size || (mode == ZSTD_e_end && !finished))
        {
            ZSTD_outBuffer outputBuf{.dst = outBuf.data(), .size = outBuf.size(), .pos = 0};
            const std::size_t remaining = ::ZSTD_compressStream2(cctx, &outputBuf, &inChunkView, mode);
            Check(!::ZSTD_isError(remaining));
            out.write(outBuf.data(), static_cast<std::streamsize>(outputBuf.pos));
            if (mode == ZSTD_e_end && remaining == 0)
            {
                finished = true;
                break;
            }
            if (mode == ZSTD_e_continue && inChunkView.pos == inChunkView.size)
            {
                break;
            }
        }
        if (mode == ZSTD_e_end)
        {
            finished = true;
        }
    }
    ::ZSTD_freeCCtx(cctx);
}

// -------- benchmarks --------

using CompressFn = void (*)(const std::filesystem::path &, const std::filesystem::path &);

// The plain JSONL file, written on first use and kept for the other benchmarks of this file.
struct PlainFixture
{
    std::unique_ptr<TestStructuredLogFile> file;
    std::size_t bytes = 0;
};

std::unique_ptr<PlainFixture> MakePlainFixture()
{
    auto fixture = std::make_unique<PlainFixture>();
    fixture->file = std::make_unique<TestStructuredLogFile>(
        StreamedRecords{
            .count = BENCH_LINE_COUNT, .seed = LARGE_FIXTURE_SEED, .timestamps = DeterministicBenchmarkTimestamps()
        },
        test_common::JsonLines(),
        test_common::RecordSchema{},
        BenchScratchPath(".jsonl")
    );
    fixture->bytes = std::filesystem::file_size(fixture->file->GetFilePath());
    return fixture;
}

using SharedPlain = std::shared_ptr<LazyInput<PlainFixture>>;

// One compressed copy of the plain file, removed when the benchmark is done.
struct CodecFixture
{
    SharedPlain plain;
    std::filesystem::path compressed;
    std::unique_ptr<FileScrubGuard> scrub;
    std::size_t compressedBytes = 0;
};

void SetDecompressionCounters(benchmark::State &state, std::size_t readBytes, std::size_t plainBytes)
{
    const auto iterations = static_cast<std::int64_t>(state.iterations());
    state.SetBytesProcessed(iterations * static_cast<std::int64_t>(readBytes));
    state.SetItemsProcessed(iterations * static_cast<std::int64_t>(BENCH_LINE_COUNT));
    constexpr double MIB = 1024.0 * 1024.0;
    state.counters["read_MiB"] = static_cast<double>(readBytes) / MIB;
    state.counters["uncompressed_MiB"] = static_cast<double>(plainBytes) / MIB;
}

void RegisterCodec(
    const std::string &name, const SharedPlain &plain, const std::string &suffix, CompressFn compress, int iterations
)
{
    RegisterLarge<CodecFixture>(
        name,
        [plain, suffix, compress]() {
            auto fixture = std::make_unique<CodecFixture>();
            fixture->plain = plain;
            const PlainFixture &source = plain->Get();
            fixture->compressed = BenchScratchPath(suffix);
            fixture->scrub = std::make_unique<FileScrubGuard>(fixture->compressed);
            compress(source.file->GetFilePath(), fixture->compressed);
            fixture->compressedBytes = std::filesystem::file_size(fixture->compressed);
            return fixture;
        },
        [](CodecFixture &fixture, benchmark::State &state, bool first) {
            const JsonParser parser;
            auto runOnce = [&]() {
                DecompressingByteSource dbs(fixture.compressed);
                if (!dbs.WasDecompressed())
                {
                    return std::size_t{0};
                }
                return ParseFile(parser, dbs.EffectivePath()).data.Lines().size();
            };
            std::size_t lines = first ? runOnce() : BENCH_LINE_COUNT;
            for (auto _ : state)
            {
                lines = runOnce();
            }
            if (lines != BENCH_LINE_COUNT)
            {
                Fail(state, "parsed " + std::to_string(lines) + " lines, expected " + std::to_string(BENCH_LINE_COUNT));
                return;
            }
            SetDecompressionCounters(state, fixture.compressedBytes, fixture.plain->Get().bytes);
        },
        LargeOptions{.iterations = iterations}
    );
}

} // namespace

void RegisterDecompressionBenchmarks()
{
    const SharedPlain plain = MakeLazy<PlainFixture>(MakePlainFixture);

    // Baseline: parser cost on the uncompressed file, for reading codec overhead off the numbers below.
    struct PlainRun
    {
        SharedPlain plain;
    };
    RegisterLarge<PlainRun>(
        "Baseline: ParseFile on uncompressed JSONL",
        [plain]() { return std::make_unique<PlainRun>(PlainRun{.plain = plain}); },
        [](PlainRun &run, benchmark::State &state, bool first) {
            const PlainFixture &fixture = run.plain->Get();
            const JsonParser parser;
            std::size_t lines =
                first ? ParseFile(parser, fixture.file->GetFilePath()).data.Lines().size() : BENCH_LINE_COUNT;
            for (auto _ : state)
            {
                lines = ParseFile(parser, fixture.file->GetFilePath()).data.Lines().size();
            }
            if (lines != BENCH_LINE_COUNT)
            {
                Fail(state, "parsed the wrong number of lines");
                return;
            }
            SetDecompressionCounters(state, fixture.bytes, fixture.bytes);
        },
        LargeOptions{.iterations = FAST_LARGE_ITERATIONS}
    );

    // The same file through the path the GUI and slp use: open, detect, parse into a typed table and finish. Its
    // figure is the reference for the table path, not a replacement for the parse-only baseline above.
    RegisterLarge<PlainRun>(
        "LoadFile on uncompressed JSONL",
        [plain]() { return std::make_unique<PlainRun>(PlainRun{.plain = plain}); },
        [](PlainRun &run, benchmark::State &state, bool first) {
            const PlainFixture &fixture = run.plain->Get();
            std::size_t rows = first ? LoadFile(fixture.file->GetFilePath()).table.RowCount() : BENCH_LINE_COUNT;
            for (auto _ : state)
            {
                const LoadResult loaded = LoadFile(fixture.file->GetFilePath());
                rows = loaded.table.RowCount();
            }
            if (rows != BENCH_LINE_COUNT)
            {
                Fail(state, "loaded the wrong number of rows");
                return;
            }
            SetDecompressionCounters(state, fixture.bytes, fixture.bytes);
        },
        LargeOptions{.iterations = FAST_LARGE_ITERATIONS}
    );

    // The iteration counts follow the cost of a run: bzip2 takes over a second, single-block xz a few hundred
    // milliseconds, the others well under 150 ms.
    RegisterCodec("Decompress + parse (gzip)", plain, ".jsonl.gz", &CompressToGzip, FAST_LARGE_ITERATIONS);
    RegisterCodec("Decompress + parse (bzip2)", plain, ".jsonl.bz2", &CompressToBzip2, 1);
    // Single-block .xz: MT decoder falls back to ST here (baseline).
    RegisterCodec("Decompress + parse (xz, single-block)", plain, ".jsonl.xz", &CompressToXz, 2);
    // Multi-block .xz: exercises the MT decoder path. Kept apart from the single-block fixture so the ST baseline stays
    // comparable.
    RegisterCodec(
        "Decompress + parse (xz, multi-block/MT)", plain, ".jsonl.mt.xz", &CompressToXzMt, FAST_LARGE_ITERATIONS
    );
    RegisterCodec("Decompress + parse (zstd)", plain, ".jsonl.zst", &CompressToZstd, FAST_LARGE_ITERATIONS);
}

} // namespace bench
