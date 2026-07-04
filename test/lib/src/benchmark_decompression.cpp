// Decompression benchmark for `loglib::internal::DecompressingByteSource`.
// Measures the end-to-end time of `DecompressingByteSource` + `ParseFile`
// (JsonParser) on a ~500 MiB JSONL fixture compressed with each codec,
// vs the uncompressed baseline. Runs only in release builds (see
// `BENCHMARK_REQUIRES_RELEASE_BUILD`) and is opt-in via the `[benchmark]`
// tag — `ctest -L benchmark` selects it.

#include "benchmark_common.hpp"
#include "common.hpp"

#include <loglib/internal/decompressing_byte_source.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/parsers/json_parser.hpp>

#include <test_common/log_format.hpp>

#include <catch2/catch_all.hpp>

#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <stdexcept>
#include <string>

using bench::RequireReleaseBuildForBenchmarks;
using bench::ReportThroughput;
using bench::RunTimedSamples;
using bench::ThroughputInputs;
using loglib::JsonParser;
using loglib::ParseFile;
using loglib::internal::DecompressingByteSource;

namespace
{

constexpr std::size_t kBenchLineCount = 5'000'000; // ~500 MiB JSONL
constexpr std::size_t kIoChunk = 256 * 1024;

/// Delete @p path (best-effort) when the RAII guard goes out of scope.
struct FileScrubGuard
{
    std::filesystem::path path;
    ~FileScrubGuard()
    {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
};

std::string BenchScratchPath(const std::string &suffix)
{
    static const std::filesystem::path scratchRoot = [] {
        auto p = std::filesystem::temp_directory_path() / "slv-bench-decompression";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p;
    }();
    return (scratchRoot / ("decompression_fixture" + suffix)).string();
}

// -------- streaming compressors (input file -> compressed output file) --------

void CompressToGzip(const std::filesystem::path &input, const std::filesystem::path &output)
{
    std::ifstream in(input, std::ios::binary);
    std::ofstream out(output, std::ios::binary);
    REQUIRE((in.is_open() && out.is_open()));

    z_stream strm{};
    REQUIRE(::deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);

    std::array<Bytef, kIoChunk> inBuf{};
    std::array<Bytef, kIoChunk> outBuf{};
    int flush = Z_NO_FLUSH;
    do
    {
        in.read(reinterpret_cast<char *>(inBuf.data()), static_cast<std::streamsize>(inBuf.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        flush = in.eof() ? Z_FINISH : Z_NO_FLUSH;
        strm.next_in = inBuf.data();
        strm.avail_in = static_cast<uInt>(got);

        do
        {
            strm.next_out = outBuf.data();
            strm.avail_out = static_cast<uInt>(outBuf.size());
            const int ret = ::deflate(&strm, flush);
            REQUIRE(ret != Z_STREAM_ERROR);
            const std::size_t produced = outBuf.size() - strm.avail_out;
            out.write(reinterpret_cast<const char *>(outBuf.data()), static_cast<std::streamsize>(produced));
        } while (strm.avail_out == 0);
    } while (flush != Z_FINISH);
    ::deflateEnd(&strm);
}

void CompressToBzip2(const std::filesystem::path &input, const std::filesystem::path &output)
{
    std::ifstream in(input, std::ios::binary);
    std::ofstream out(output, std::ios::binary);
    REQUIRE((in.is_open() && out.is_open()));

    bz_stream strm{};
    REQUIRE(::BZ2_bzCompressInit(&strm, 9, 0, 30) == BZ_OK);

    std::array<char, kIoChunk> inBuf{};
    std::array<char, kIoChunk> outBuf{};
    int action = BZ_RUN;
    // Loop until `BZ2_bzCompress` reports `BZ_STREAM_END`. Do NOT
    // exit early on `avail_in == 0` in FINISH mode -- bzip2 keeps
    // returning `BZ_FINISH_OK` (not `BZ_STREAM_END`) while it
    // flushes internally buffered output, and the final flush can
    // exceed our 256 KiB output buffer for large inputs. Exiting
    // when `avail_in` hits zero would silently truncate the file.
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
        REQUIRE((ret == BZ_RUN_OK || ret == BZ_FINISH_OK || ret == BZ_STREAM_END));
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
    REQUIRE((in.is_open() && out.is_open()));

    lzma_stream strm = LZMA_STREAM_INIT;
    // Preset 1 keeps compression time reasonable for a 500 MiB fixture.
    REQUIRE(::lzma_easy_encoder(&strm, 1, LZMA_CHECK_CRC64) == LZMA_OK);

    std::array<std::uint8_t, kIoChunk> inBuf{};
    std::array<std::uint8_t, kIoChunk> outBuf{};
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
        REQUIRE((ret == LZMA_OK || ret == LZMA_STREAM_END));
        const std::size_t produced = outBuf.size() - strm.avail_out;
        out.write(reinterpret_cast<const char *>(outBuf.data()), static_cast<std::streamsize>(produced));
    }
    ::lzma_end(&strm);
}

void CompressToZstd(const std::filesystem::path &input, const std::filesystem::path &output)
{
    std::ifstream in(input, std::ios::binary);
    std::ofstream out(output, std::ios::binary);
    REQUIRE((in.is_open() && out.is_open()));

    ZSTD_CCtx *cctx = ::ZSTD_createCCtx();
    REQUIRE(cctx != nullptr);

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
        ZSTD_inBuffer inChunkView{inBuf.data(), got, 0};
        while (inChunkView.pos < inChunkView.size || (mode == ZSTD_e_end && !finished))
        {
            ZSTD_outBuffer outputBuf{outBuf.data(), outBuf.size(), 0};
            const std::size_t remaining = ::ZSTD_compressStream2(cctx, &outputBuf, &inChunkView, mode);
            REQUIRE(!::ZSTD_isError(remaining));
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

// -------- benchmark helper --------

struct FixtureLocations
{
    std::filesystem::path uncompressed;
    std::filesystem::path gzip;
    std::filesystem::path bzip2;
    std::filesystem::path xz;
    std::filesystem::path zstd;
};

FixtureLocations BuildFixtures()
{
    FixtureLocations paths;
    paths.uncompressed = BenchScratchPath(".jsonl");
    paths.gzip = BenchScratchPath(".jsonl.gz");
    paths.bzip2 = BenchScratchPath(".jsonl.bz2");
    paths.xz = BenchScratchPath(".jsonl.xz");
    paths.zstd = BenchScratchPath(".jsonl.zst");

    // Regenerate the uncompressed fixture every run so the on-disk size
    // is deterministic across CI hosts (and always reflects the shape of
    // `TestStructuredLogFile`'s streaming ctor).
    TestStructuredLogFile(
        StreamedRecords{
            .count = kBenchLineCount,
            .seed = bench::LARGE_FIXTURE_SEED,
            .timestamps = bench::DeterministicBenchmarkTimestamps()
        },
        test_common::JsonLines(),
        {}, // default schema
        paths.uncompressed.string()
    );

    // Compress once per codec. Skip when a stale artefact of the same size
    // is already present — makes local iteration cheap after the first run.
    auto ensureCompressed = [](const std::filesystem::path &input,
                               const std::filesystem::path &output,
                               void (*compress)(const std::filesystem::path &, const std::filesystem::path &)) {
        if (std::filesystem::exists(output) && std::filesystem::file_size(output) > 0)
        {
            return;
        }
        compress(input, output);
    };
    ensureCompressed(paths.uncompressed, paths.gzip, &CompressToGzip);
    ensureCompressed(paths.uncompressed, paths.bzip2, &CompressToBzip2);
    ensureCompressed(paths.uncompressed, paths.xz, &CompressToXz);
    ensureCompressed(paths.uncompressed, paths.zstd, &CompressToZstd);
    return paths;
}

std::size_t TimeAndReport(const char *label, const std::filesystem::path &path, bool decompressFirst)
{
    const std::size_t bytes = std::filesystem::file_size(path);
    const JsonParser parser;
    const auto start = std::chrono::steady_clock::now();
    if (decompressFirst)
    {
        DecompressingByteSource dbs(path);
        REQUIRE(dbs.WasDecompressed());
        const auto result = ParseFile(parser, dbs.EffectivePath());
        REQUIRE(result.data.Lines().size() == kBenchLineCount);
    }
    else
    {
        const auto result = ParseFile(parser, path);
        REQUIRE(result.data.Lines().size() == kBenchLineCount);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    ReportThroughput(label, elapsed, bytes, kBenchLineCount);
    return bytes;
}

} // namespace

TEST_CASE("Decompress + parse a 500 MiB JSONL fixture (all codecs)", "[.][benchmark][decompression]")
{
    BENCHMARK_REQUIRES_RELEASE_BUILD();

    const FixtureLocations paths = BuildFixtures();

    // Baseline: parse the uncompressed file directly. This calibrates
    // the parser cost so codec overhead is easy to read off the numbers.
    const std::size_t baselineBytes = TimeAndReport("Baseline: ParseFile on uncompressed JSONL", paths.uncompressed, false);
    (void)baselineBytes;

    const auto compressedBytes = std::filesystem::file_size(paths.gzip);
    WARN(
        "Fixture sizes: uncompressed=" << baselineBytes / (1024 * 1024) << " MiB, gzip=" << compressedBytes / (1024 * 1024) << " MiB"
    );

    TimeAndReport("Decompress + parse (gzip)", paths.gzip, true);
    TimeAndReport("Decompress + parse (bzip2)", paths.bzip2, true);
    TimeAndReport("Decompress + parse (xz)", paths.xz, true);
    TimeAndReport("Decompress + parse (zstd)", paths.zstd, true);
}
