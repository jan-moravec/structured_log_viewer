#include "common.hpp"

#include <loglib/internal/decompressing_byte_source.hpp>
#include <loglib/parse_file.hpp>
#include <loglib/parsers/json_parser.hpp>
#include <loglib/stop_token.hpp>

#include <catch2/catch_all.hpp>

#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using loglib::StopSource;
using loglib::StopToken;
using loglib::internal::DecompressingByteSource;
using loglib::internal::DecompressionCancelled;

namespace
{

/// Small RAII fixture: a unique on-disk path that gets removed on
/// destruction. Simpler than `TestLogFile` because we write raw bytes
/// (compressed streams) rather than newline-terminated text.
class TempBinaryFile
{
public:
    explicit TempBinaryFile(const std::string &suffix)
    {
        std::random_device rd;
        const std::uint64_t r = (static_cast<std::uint64_t>(rd()) << 32) | rd();
        std::ostringstream name;
        name << "slv-test-" << std::hex << r << suffix;
        mPath = std::filesystem::temp_directory_path() / name.str();
    }

    ~TempBinaryFile()
    {
        std::error_code ec;
        std::filesystem::remove(mPath, ec);
    }

    TempBinaryFile(const TempBinaryFile &) = delete;
    TempBinaryFile &operator=(const TempBinaryFile &) = delete;

    const std::filesystem::path &Path() const noexcept
    {
        return mPath;
    }

    void WriteBytes(const std::vector<std::uint8_t> &bytes) const
    {
        std::ofstream out(mPath, std::ios::binary);
        REQUIRE(out.is_open());
        out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }

    std::vector<std::uint8_t> ReadBytes() const
    {
        std::ifstream in(mPath, std::ios::binary);
        REQUIRE(in.is_open());
        return std::vector<std::uint8_t>(
            (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()
        );
    }

private:
    std::filesystem::path mPath;
};

// --- reference in-test compressors --------------------------------------

std::vector<std::uint8_t> CompressGzip(const std::string &input)
{
    z_stream strm{};
    // `windowBits = 15 + 16` selects gzip framing (raw deflate + gzip header).
    REQUIRE(::deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    std::vector<std::uint8_t> out;
    out.resize(::deflateBound(&strm, static_cast<uLong>(input.size())));
    // zlib's `deflate` takes non-const `next_in` even though it doesn't
    // mutate the input. Older headers require the cast; new ones just
    // discard the const in an implicit conversion. Keep the cast for portability.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    strm.next_in = const_cast<Bytef *>(reinterpret_cast<const Bytef *>(input.data()));
    strm.avail_in = static_cast<uInt>(input.size());
    strm.next_out = out.data();
    strm.avail_out = static_cast<uInt>(out.size());
    const int rc = ::deflate(&strm, Z_FINISH);
    REQUIRE(rc == Z_STREAM_END);
    out.resize(strm.total_out);
    ::deflateEnd(&strm);
    return out;
}

std::vector<std::uint8_t> CompressBzip2(const std::string &input)
{
    // BZ2_bzBuffToBuffCompress needs at least 1% + 600 bytes headroom.
    std::vector<std::uint8_t> out;
    unsigned int outLen = static_cast<unsigned int>(input.size() + input.size() / 100 + 800);
    out.resize(outLen);
    const int rc = ::BZ2_bzBuffToBuffCompress(
        reinterpret_cast<char *>(out.data()),
        &outLen,
        const_cast<char *>(input.data()),
        static_cast<unsigned int>(input.size()),
        9, // block size 900 KB
        0, // silent
        30 // work factor
    );
    REQUIRE(rc == BZ_OK);
    out.resize(outLen);
    return out;
}

std::vector<std::uint8_t> CompressXz(const std::string &input)
{
    lzma_stream strm = LZMA_STREAM_INIT;
    REQUIRE(::lzma_easy_encoder(&strm, 3, LZMA_CHECK_CRC64) == LZMA_OK);
    std::vector<std::uint8_t> out;
    out.resize(input.size() + 1024);
    strm.next_in = reinterpret_cast<const std::uint8_t *>(input.data());
    strm.avail_in = input.size();
    strm.next_out = out.data();
    strm.avail_out = out.size();
    lzma_ret rc = LZMA_OK;
    while (rc != LZMA_STREAM_END)
    {
        if (strm.avail_out == 0)
        {
            const std::size_t was = out.size();
            out.resize(was * 2);
            strm.next_out = out.data() + was;
            strm.avail_out = was;
        }
        rc = ::lzma_code(&strm, LZMA_FINISH);
        REQUIRE((rc == LZMA_OK || rc == LZMA_STREAM_END));
    }
    out.resize(strm.total_out);
    ::lzma_end(&strm);
    return out;
}

std::vector<std::uint8_t> CompressZstd(const std::string &input)
{
    const std::size_t bound = ::ZSTD_compressBound(input.size());
    std::vector<std::uint8_t> out(bound);
    const std::size_t written = ::ZSTD_compress(out.data(), bound, input.data(), input.size(), 3);
    REQUIRE(!::ZSTD_isError(written));
    out.resize(written);
    return out;
}

std::string SampleContent(std::size_t targetBytes)
{
    std::string out;
    out.reserve(targetBytes);
    std::mt19937 gen(42);
    while (out.size() < targetBytes)
    {
        // Mix of repetition (compression-friendly) and pseudo-random
        // (compression-resistant) so all codecs produce non-trivial
        // output. Each "record" is a JSONL-style line so integration
        // tests can reuse the same shape.
        out += "{\"index\":";
        out += std::to_string(gen());
        out += ",\"msg\":\"decompression fixture line - hello world hello world\"}\n";
    }
    return out;
}

std::string ReadFileContents(const std::filesystem::path &path)
{
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in.is_open());
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("DecompressingByteSource: passthrough for uncompressed input", "[DecompressingByteSource]")
{
    const TempBinaryFile fixture(".log");
    const std::string content = SampleContent(4 * 1024);
    {
        std::ofstream out(fixture.Path(), std::ios::binary);
        REQUIRE(out.is_open());
        out << content;
    }

    DecompressingByteSource dbs(fixture.Path());

    CHECK(dbs.DisplayPath() == fixture.Path());
    CHECK(dbs.EffectivePath() == fixture.Path());
    CHECK_FALSE(dbs.WasDecompressed());
    CHECK(dbs.DetectedCodec() == DecompressingByteSource::Codec::None);
    CHECK(dbs.CompressedSize() == content.size());
    CHECK(dbs.DecompressedSize() == 0);
}

TEST_CASE("DecompressingByteSource: passthrough for empty file", "[DecompressingByteSource]")
{
    const TempBinaryFile fixture(".log");
    fixture.WriteBytes({});

    DecompressingByteSource dbs(fixture.Path());

    CHECK_FALSE(dbs.WasDecompressed());
    CHECK(dbs.DetectedCodec() == DecompressingByteSource::Codec::None);
    CHECK(dbs.EffectivePath() == fixture.Path());
    CHECK(dbs.CompressedSize() == 0);
}

TEST_CASE("DecompressingByteSource: round-trip each codec", "[DecompressingByteSource]")
{
    const std::string content = SampleContent(256 * 1024); // 256 KiB
    auto verifyRoundTrip = [&content](
                               DecompressingByteSource::Codec expected,
                               const std::vector<std::uint8_t> &compressed,
                               const std::string &suffix
                           ) {
        const TempBinaryFile fixture(suffix);
        fixture.WriteBytes(compressed);

        DecompressingByteSource dbs(fixture.Path());
        REQUIRE(dbs.WasDecompressed());
        CHECK(dbs.DetectedCodec() == expected);
        CHECK(dbs.DisplayPath() == fixture.Path());
        CHECK(dbs.EffectivePath() != fixture.Path());
        CHECK(std::filesystem::exists(dbs.EffectivePath()));
        CHECK(dbs.CompressedSize() == compressed.size());
        CHECK(dbs.DecompressedSize() == content.size());

        const std::string decoded = ReadFileContents(dbs.EffectivePath());
        CHECK(decoded == content);
    };

    SECTION("gzip")
    {
        verifyRoundTrip(DecompressingByteSource::Codec::Gzip, CompressGzip(content), ".log.gz");
    }
    SECTION("bzip2")
    {
        verifyRoundTrip(DecompressingByteSource::Codec::Bzip2, CompressBzip2(content), ".log.bz2");
    }
    SECTION("xz")
    {
        verifyRoundTrip(DecompressingByteSource::Codec::Xz, CompressXz(content), ".log.xz");
    }
    SECTION("zstd")
    {
        verifyRoundTrip(DecompressingByteSource::Codec::Zstd, CompressZstd(content), ".log.zst");
    }
}

TEST_CASE("DecompressingByteSource: detects codec by content, not extension", "[DecompressingByteSource]")
{
    const std::string content = SampleContent(4 * 1024);
    const auto compressed = CompressGzip(content);

    // Extension deliberately not .gz — sniffing must still detect gzip.
    const TempBinaryFile fixture(".log");
    fixture.WriteBytes(compressed);

    DecompressingByteSource dbs(fixture.Path());
    CHECK(dbs.DetectedCodec() == DecompressingByteSource::Codec::Gzip);
    CHECK(dbs.WasDecompressed());
    CHECK(ReadFileContents(dbs.EffectivePath()) == content);
}

TEST_CASE("DecompressingByteSource: multi-member gzip stream", "[DecompressingByteSource]")
{
    const std::string first = "member-one\n";
    const std::string second = "member-two\n";
    auto a = CompressGzip(first);
    const auto b = CompressGzip(second);
    a.insert(a.end(), b.begin(), b.end());

    const TempBinaryFile fixture(".log.gz");
    fixture.WriteBytes(a);

    DecompressingByteSource dbs(fixture.Path());
    CHECK(dbs.DetectedCodec() == DecompressingByteSource::Codec::Gzip);
    CHECK(ReadFileContents(dbs.EffectivePath()) == first + second);
}

TEST_CASE("DecompressingByteSource: truncated codec input surfaces an error", "[DecompressingByteSource]")
{
    const std::string content = SampleContent(64 * 1024);

    auto verifyTruncationThrows = [&content](
                                      const std::vector<std::uint8_t> &compressed,
                                      const std::string &suffix,
                                      std::size_t truncatedSize
                                  ) {
        REQUIRE(compressed.size() > truncatedSize);
        std::vector<std::uint8_t> truncated(compressed.begin(), compressed.begin() + truncatedSize);

        const TempBinaryFile fixture(suffix);
        fixture.WriteBytes(truncated);

        std::filesystem::path effectiveBefore;
        CHECK_THROWS_AS(
            [&]() {
                DecompressingByteSource dbs(fixture.Path());
                effectiveBefore = dbs.EffectivePath();
            }(),
            std::runtime_error
        );
        // We can't observe `EffectivePath()` after the throw, but the
        // internal cleanup contract says any partial temp must be gone.
        // Scanning the temp dir for `slv-decompressed-*.tmp` isn't
        // reliable in parallel test runs; instead we simply confirm
        // the source file is intact.
        CHECK(std::filesystem::file_size(fixture.Path()) == truncatedSize);
    };

    SECTION("gzip")
    {
        verifyTruncationThrows(CompressGzip(content), ".log.gz", 32);
    }
    SECTION("bzip2")
    {
        verifyTruncationThrows(CompressBzip2(content), ".log.bz2", 32);
    }
    SECTION("xz")
    {
        verifyTruncationThrows(CompressXz(content), ".log.xz", 32);
    }
    SECTION("zstd")
    {
        verifyTruncationThrows(CompressZstd(content), ".log.zst", 32);
    }
}

TEST_CASE("DecompressingByteSource: corrupted codec input surfaces an error", "[DecompressingByteSource]")
{
    const std::string content = SampleContent(64 * 1024);

    auto verifyCorruptionThrows = [](std::vector<std::uint8_t> bytes, const std::string &suffix) {
        // Flip bytes past the magic prefix so detection still triggers
        // the codec, but the payload is invalid. The flip site is well
        // inside the payload (offset 24) so it's safely past every
        // codec's magic bytes and container header prefix.
        REQUIRE(bytes.size() > 32);
        bytes[24] ^= 0xFF;
        bytes[25] ^= 0xFF;
        bytes[26] ^= 0xFF;

        const TempBinaryFile fixture(suffix);
        fixture.WriteBytes(bytes);

        CHECK_THROWS_AS(DecompressingByteSource(fixture.Path()), std::runtime_error);
    };

    SECTION("gzip")
    {
        verifyCorruptionThrows(CompressGzip(content), ".log.gz");
    }
    SECTION("bzip2")
    {
        verifyCorruptionThrows(CompressBzip2(content), ".log.bz2");
    }
    SECTION("xz")
    {
        verifyCorruptionThrows(CompressXz(content), ".log.xz");
    }
    SECTION("zstd")
    {
        verifyCorruptionThrows(CompressZstd(content), ".log.zst");
    }
}

TEST_CASE("DecompressingByteSource: destructor removes the temp file", "[DecompressingByteSource]")
{
    const auto compressed = CompressGzip(SampleContent(16 * 1024));
    const TempBinaryFile fixture(".log.gz");
    fixture.WriteBytes(compressed);

    std::filesystem::path effective;
    {
        DecompressingByteSource dbs(fixture.Path());
        effective = dbs.EffectivePath();
        REQUIRE(std::filesystem::exists(effective));
    }
    CHECK_FALSE(std::filesystem::exists(effective));
}

TEST_CASE("DecompressingByteSource: progress callback observations", "[DecompressingByteSource]")
{
    // Use ~4 MB of compressed data so we get at least 60 chunks of
    // 64 KiB. That gives us enough progress firings to see the
    // monotonic property clearly.
    const auto compressed = CompressGzip(SampleContent(4 * 1024 * 1024));
    const TempBinaryFile fixture(".log.gz");
    fixture.WriteBytes(compressed);

    std::size_t callCount = 0;
    std::size_t lastBytesIn = 0;
    std::size_t observedTotal = 0;

    DecompressingByteSource dbs(
        fixture.Path(),
        [&](const DecompressingByteSource::Progress &p) {
            ++callCount;
            CHECK(p.bytesIn >= lastBytesIn);
            lastBytesIn = p.bytesIn;
            observedTotal = p.totalBytesIn;
        }
    );

    CHECK(callCount >= 1);
    CHECK(lastBytesIn == compressed.size());
    CHECK(observedTotal == compressed.size());
}

TEST_CASE("DecompressingByteSource: StopToken cancels mid-decompress", "[DecompressingByteSource]")
{
    // Large fixture so the worker has enough work for the cancel to
    // land before it naturally completes.
    const auto compressed = CompressGzip(SampleContent(32 * 1024 * 1024));
    const TempBinaryFile fixture(".log.gz");
    fixture.WriteBytes(compressed);

    StopSource stopSource;

    std::atomic<bool> observedFirstProgress{false};

    // Kick the stop from a helper thread as soon as the worker
    // reports its first progress tick — this guarantees we cancel
    // mid-decompress, not before.
    auto callback = [&](const DecompressingByteSource::Progress &) {
        observedFirstProgress.store(true, std::memory_order_release);
    };
    std::thread canceller([&stopSource, &observedFirstProgress]() {
        while (!observedFirstProgress.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        stopSource.request_stop();
    });

    CHECK_THROWS_AS(
        DecompressingByteSource(fixture.Path(), callback, stopSource.get_token()), DecompressionCancelled
    );

    canceller.join();

    // Cleanup contract: any temp file created for this decompress
    // must have been removed. We can't peek at it directly, but we
    // can verify the source file is intact.
    CHECK(std::filesystem::file_size(fixture.Path()) == compressed.size());
}

TEST_CASE("DecompressingByteSource: CodecName maps every enum value", "[DecompressingByteSource]")
{
    using Codec = DecompressingByteSource::Codec;
    CHECK(loglib::internal::CodecName(Codec::None) == "none");
    CHECK(loglib::internal::CodecName(Codec::Gzip) == "gzip");
    CHECK(loglib::internal::CodecName(Codec::Bzip2) == "bzip2");
    CHECK(loglib::internal::CodecName(Codec::Xz) == "xz");
    CHECK(loglib::internal::CodecName(Codec::Zstd) == "zstd");
}

// -----------------------------------------------------------------------
// Integration test: compressed input -> DecompressingByteSource ->
// ParseFile(JsonParser). Row count and error count must match the
// uncompressed reference. This mirrors the app-level flow that
// MainWindow uses (sniff + decompress + LogFile + parse).
// -----------------------------------------------------------------------

TEST_CASE("DecompressingByteSource: row-count parity with ParseFile", "[DecompressingByteSource][ParseFile]")
{
    constexpr std::size_t kLineCount = 5000;
    std::string jsonl;
    jsonl.reserve(kLineCount * 96);
    for (std::size_t i = 0; i < kLineCount; ++i)
    {
        jsonl += "{\"index\":";
        jsonl += std::to_string(i);
        jsonl += ",\"level\":\"info\",\"msg\":\"decompression parity fixture line\"}\n";
    }

    // Reference uncompressed parse.
    const TempBinaryFile plainFile(".jsonl");
    plainFile.WriteBytes(std::vector<std::uint8_t>(jsonl.begin(), jsonl.end()));

    const loglib::JsonParser parser;
    const auto reference = loglib::ParseFile(parser, plainFile.Path());
    REQUIRE(reference.data.Lines().size() == kLineCount);
    REQUIRE(reference.errors.empty());

    auto verifyParity = [&](const std::vector<std::uint8_t> &compressed, const std::string &suffix) {
        const TempBinaryFile fixture(suffix);
        fixture.WriteBytes(compressed);

        DecompressingByteSource dbs(fixture.Path());
        REQUIRE(dbs.WasDecompressed());

        const auto result = loglib::ParseFile(parser, dbs.EffectivePath());
        CHECK(result.data.Lines().size() == reference.data.Lines().size());
        CHECK(result.errors.size() == reference.errors.size());
    };

    SECTION("gzip")
    {
        verifyParity(CompressGzip(jsonl), ".jsonl.gz");
    }
    SECTION("bzip2")
    {
        verifyParity(CompressBzip2(jsonl), ".jsonl.bz2");
    }
    SECTION("xz")
    {
        verifyParity(CompressXz(jsonl), ".jsonl.xz");
    }
    SECTION("zstd")
    {
        verifyParity(CompressZstd(jsonl), ".jsonl.zst");
    }
}
