#include "common.hpp"

#include <loglib/internal/decompressing_byte_source.hpp>
#include <loglib/log_file.hpp>
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
#include <system_error>
#include <thread>
#include <type_traits>
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

// Large multi-member streams: each member is big enough that the
// decoder's 64 KiB read/output chunking straddles member boundaries,
// so this exercises the concatenation reset *and* the pending-output
// drain paths (a single compressed chunk can span the end of one
// member and the start of the next, and a member's decompressed output
// spans many output buffers). The tiny multi-member test above stays in
// a single chunk and would not catch a boundary regression.
TEST_CASE("DecompressingByteSource: large multi-member gzip stream", "[DecompressingByteSource]")
{
    const std::string first = SampleContent(200 * 1024);
    const std::string second = SampleContent(150 * 1024);
    const std::string third = SampleContent(300 * 1024);
    std::vector<std::uint8_t> combined = CompressGzip(first);
    for (const auto &member : {second, third})
    {
        const auto part = CompressGzip(member);
        combined.insert(combined.end(), part.begin(), part.end());
    }

    const TempBinaryFile fixture(".log.gz");
    fixture.WriteBytes(combined);

    DecompressingByteSource dbs(fixture.Path());
    CHECK(dbs.DetectedCodec() == DecompressingByteSource::Codec::Gzip);
    CHECK(dbs.DecompressedSize() == first.size() + second.size() + third.size());
    CHECK(ReadFileContents(dbs.EffectivePath()) == first + second + third);
}

TEST_CASE("DecompressingByteSource: large multi-member bzip2 stream", "[DecompressingByteSource]")
{
    const std::string first = SampleContent(200 * 1024);
    const std::string second = SampleContent(150 * 1024);
    const std::string third = SampleContent(300 * 1024);
    std::vector<std::uint8_t> combined = CompressBzip2(first);
    for (const auto &member : {second, third})
    {
        const auto part = CompressBzip2(member);
        combined.insert(combined.end(), part.begin(), part.end());
    }

    const TempBinaryFile fixture(".log.bz2");
    fixture.WriteBytes(combined);

    DecompressingByteSource dbs(fixture.Path());
    CHECK(dbs.DetectedCodec() == DecompressingByteSource::Codec::Bzip2);
    CHECK(dbs.DecompressedSize() == first.size() + second.size() + third.size());
    CHECK(ReadFileContents(dbs.EffectivePath()) == first + second + third);
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

    // Snapshot the temp directory contents before the cancel so we
    // can prove the partial temp file the worker created got
    // cleaned up (see below). We compare by name pattern rather
    // than by exact set to avoid false positives from unrelated
    // temp files created by other processes on shared CI hosts.
    const std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    auto countSlvTemps = [&]() {
        std::size_t n = 0;
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(tempDir, ec))
        {
            if (entry.path().filename().string().rfind("slv-decompressed-", 0) == 0)
            {
                ++n;
            }
        }
        return n;
    };
    const std::size_t beforeSlvTemps = countSlvTemps();

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
    // must have been removed. Source file must also be intact.
    CHECK(std::filesystem::file_size(fixture.Path()) == compressed.size());
    CHECK(countSlvTemps() == beforeSlvTemps);
}

TEST_CASE(
    "DecompressingByteSource: DecompressionCancelled is not a std::runtime_error",
    "[DecompressingByteSource]"
)
{
    // Regression guard: `MainWindow::OnDecompressionFinished`
    // relies on `catch (DecompressionCancelled)` firing before
    // `catch (std::exception)` so cancels surface as a toast and
    // codec errors surface as a modal batch. Historically
    // `DecompressionCancelled` derived from `std::runtime_error`,
    // which meant any older catch-`std::runtime_error` site would
    // silently absorb cancels as codec errors. Lock the intent in
    // with a static type-relation assertion so a future refactor
    // that re-parents the class trips this test before any
    // downstream catches misclassify.
    static_assert(!std::is_base_of_v<std::runtime_error, DecompressionCancelled>);
    static_assert(std::is_base_of_v<std::exception, DecompressionCancelled>);
    SUCCEED();
}

TEST_CASE("DecompressingByteSource: zstd skippable-frame prefix detected", "[DecompressingByteSource]")
{
    // RFC 8878 §3.1.2: skippable frames start with a 32-bit LE
    // magic in `[0x184D2A50, 0x184D2A5F]` followed by a 32-bit LE
    // frame-content size, then that many arbitrary bytes. A valid
    // zstd stream can begin with any number of these before its
    // first regular frame; producers use them to embed metadata
    // (dictionary IDs, custom headers). Historically we sniffed
    // only the regular frame magic (`0x28 0xB5 0x2F 0xFD`) and
    // silently dropped these inputs to the plain-text path,
    // where the raw compressed bytes would then flow into the
    // JSONL parser. Regression fixture: prepend a 4-byte payload
    // skippable frame to a real zstd stream and confirm SniffCodec
    // + full decompress recover the underlying payload.
    const std::string payload = SampleContent(4 * 1024);
    const auto zstd = CompressZstd(payload);

    std::vector<std::uint8_t> withSkippable;
    withSkippable.reserve(zstd.size() + 12);
    // Skippable magic (bit 0..3 pick a variant; use 0x50 which
    // is the "reserved" byte pattern in libzstd's own test corpus).
    withSkippable.push_back(0x50);
    withSkippable.push_back(0x2A);
    withSkippable.push_back(0x4D);
    withSkippable.push_back(0x18);
    // Skippable frame body length: 4 bytes little-endian.
    withSkippable.push_back(0x04);
    withSkippable.push_back(0x00);
    withSkippable.push_back(0x00);
    withSkippable.push_back(0x00);
    // Skippable frame body: 4 arbitrary bytes.
    withSkippable.push_back(0xDE);
    withSkippable.push_back(0xAD);
    withSkippable.push_back(0xBE);
    withSkippable.push_back(0xEF);
    // Real zstd frame appended.
    withSkippable.insert(withSkippable.end(), zstd.begin(), zstd.end());

    const TempBinaryFile fixture(".log.zst");
    fixture.WriteBytes(withSkippable);

    CHECK(DecompressingByteSource::SniffCodec(fixture.Path()) == DecompressingByteSource::Codec::Zstd);

    DecompressingByteSource dbs(fixture.Path());
    REQUIRE(dbs.WasDecompressed());
    REQUIRE(dbs.DetectedCodec() == DecompressingByteSource::Codec::Zstd);
    CHECK(ReadFileContents(dbs.EffectivePath()) == payload);
}

TEST_CASE("DecompressingByteSource: max decompressed size cap trips", "[DecompressingByteSource]")
{
    // Force a size-cap trip on a small but highly-compressible
    // gzip payload. Cap set below the payload size so the check
    // fires deterministically after the first output chunk.
    const std::string payload = SampleContent(256 * 1024);
    const auto compressed = CompressGzip(payload);
    const TempBinaryFile fixture(".log.gz");
    fixture.WriteBytes(compressed);

    const std::filesystem::path tempDir = std::filesystem::temp_directory_path();
    auto countSlvTemps = [&]() {
        std::size_t n = 0;
        std::error_code ec;
        for (const auto &entry : std::filesystem::directory_iterator(tempDir, ec))
        {
            if (entry.path().filename().string().rfind("slv-decompressed-", 0) == 0)
            {
                ++n;
            }
        }
        return n;
    };
    const std::size_t beforeSlvTemps = countSlvTemps();

    DecompressingByteSource::Options options;
    // 16 KiB cap << 256 KiB payload -> guaranteed trip on the
    // first 64 KiB output chunk.
    options.maxDecompressedBytes = 16 * 1024;

    CHECK_THROWS_AS(
        DecompressingByteSource(fixture.Path(), {}, {}, options),
        loglib::internal::DecompressionSizeCapExceeded
    );

    // The partial temp file must be cleaned up on the exception
    // unwind, same contract as the cancel path.
    CHECK(countSlvTemps() == beforeSlvTemps);
}

TEST_CASE(
    "DecompressingByteSource: max decompressed size cap of 0 is disabled",
    "[DecompressingByteSource]"
)
{
    // A 0 cap must NOT throw: it disables enforcement entirely,
    // per the ctor Options doc. Regression guard against a
    // future accidental `> 0` -> `>= 0` off-by-one that would
    // reject every decompress.
    const std::string payload = SampleContent(4 * 1024);
    const auto compressed = CompressGzip(payload);
    const TempBinaryFile fixture(".log.gz");
    fixture.WriteBytes(compressed);

    DecompressingByteSource::Options options;
    options.maxDecompressedBytes = 0;
    DecompressingByteSource dbs(fixture.Path(), {}, {}, options);
    REQUIRE(dbs.WasDecompressed());
    CHECK(ReadFileContents(dbs.EffectivePath()) == payload);
}

TEST_CASE("DecompressingByteSource: move-construct transfers temp-file ownership", "[DecompressingByteSource]")
{
    // Regression guard for the move-ctor path: the moved-into
    // object must own the temp file and the moved-from object
    // must NOT unlink it in its destructor. Historical bug shape:
    // a shallow move that left both objects with
    // `mOwnsTempFile = true` would double-delete the temp path on
    // scope exit; a swap-based move that forgot to clear the
    // source's ownership flag would let the source's dtor unlink
    // the file the destination was still reading.
    const std::string payload = SampleContent(64 * 1024);
    const auto compressed = CompressGzip(payload);
    const TempBinaryFile fixture(".log.gz");
    fixture.WriteBytes(compressed);

    std::filesystem::path effective;
    {
        DecompressingByteSource src(fixture.Path());
        REQUIRE(src.WasDecompressed());
        effective = src.EffectivePath();
        REQUIRE(std::filesystem::exists(effective));

        DecompressingByteSource dst(std::move(src));
        // After the move, the destination owns the file and can
        // still see it on disk. When `src`'s destructor runs at
        // the end of this inner scope it must NOT unlink the file
        // that ownership has transferred to `dst`.
        CHECK(dst.EffectivePath() == effective);
        CHECK(std::filesystem::exists(effective));
    }
    // Both DBS objects destroyed; the surviving owner (dst) has
    // now also gone, so the temp file must be cleaned up.
    CHECK(!std::filesystem::exists(effective));
}

TEST_CASE("DecompressingByteSource: move-assign releases previous temp file", "[DecompressingByteSource]")
{
    // Companion to the move-ctor test: move-assignment must
    // release the destination's *existing* temp file before it
    // adopts the source's, otherwise a second decompress would
    // leak the first one to the temp directory.
    const std::string payload = SampleContent(64 * 1024);
    const auto compressed = CompressGzip(payload);
    const TempBinaryFile fixture(".log.gz");
    fixture.WriteBytes(compressed);

    std::filesystem::path srcEffective;
    std::filesystem::path dstOriginal;
    {
        DecompressingByteSource src(fixture.Path());
        REQUIRE(src.WasDecompressed());
        srcEffective = src.EffectivePath();
        REQUIRE(std::filesystem::exists(srcEffective));

        DecompressingByteSource dst(fixture.Path());
        dstOriginal = dst.EffectivePath();
        REQUIRE(std::filesystem::exists(dstOriginal));
        REQUIRE(dstOriginal != srcEffective);

        dst = std::move(src);
        CHECK(!std::filesystem::exists(dstOriginal)); // released
        CHECK(dst.EffectivePath() == srcEffective);   // adopted
        CHECK(std::filesystem::exists(srcEffective));
    }
    CHECK(!std::filesystem::exists(srcEffective));
    CHECK(!std::filesystem::exists(dstOriginal));
}

TEST_CASE(
    "DecompressingByteSource: LogFile::AttachLifetimeAnchor unlinks temp on LogFile destruction",
    "[DecompressingByteSource][LogFile][anchor-lifetime]"
)
{
    // Mirrors the production `MainWindow::ContinueOpenAfterPrepared`
    // flow: after a successful decompress, the app wraps the
    // `DecompressingByteSource` in a shared_ptr and attaches it as
    // a lifetime anchor to the `LogFile` that maps its temp file.
    // Correct ordering (anchor destroyed AFTER `mMmap` unmaps) is
    // required so the OS-level file lock on the temp path is
    // released *before* the unlink attempt on Windows, which
    // otherwise silently no-ops and leaks the temp file. Peg the
    // contract with a real DBS + real LogFile.
    const std::string payload = SampleContent(4 * 1024);
    const auto compressed = CompressGzip(payload);
    const TempBinaryFile fixture(".log.gz");
    fixture.WriteBytes(compressed);

    std::filesystem::path tempPath;
    {
        auto dbs = std::make_shared<DecompressingByteSource>(fixture.Path());
        REQUIRE(dbs->WasDecompressed());
        tempPath = dbs->EffectivePath();
        REQUIRE(std::filesystem::exists(tempPath));

        auto logFile = std::make_unique<loglib::LogFile>(tempPath);
        logFile->AttachLifetimeAnchor(std::move(dbs));

        // While the LogFile is alive, the temp file must remain
        // (the mmap holds a handle into it).
        CHECK(std::filesystem::exists(tempPath));
    }
    // LogFile destroyed. mmap unmapped first (member order in
    // LogFile), then the composed anchor's dtor ran the DBS's
    // ReleaseTempFile(), which unlinked the temp file.
    CHECK(!std::filesystem::exists(tempPath));
}

TEST_CASE("DecompressingByteSource::SniffCodec: empty and missing paths report Codec::None", "[DecompressingByteSource]")
{
    // `SniffCodec` is the pre-flight gate `MainWindow::StreamNextPendingFile`
    // uses to decide "sync fast path vs async worker" before it commits to a
    // `QtConcurrent::run` dispatch. It must be noexcept-safe on any input the
    // OS lets us pass, and any I/O failure has to collapse to `Codec::None`
    // so the downstream `LogFile` ctor produces the canonical
    // open-error message rather than a duplicate diagnostic here.
    using Codec = DecompressingByteSource::Codec;

    // Empty file: 0 bytes can't hold any codec's frame header.
    const TempBinaryFile empty(".log");
    empty.WriteBytes({});
    CHECK(DecompressingByteSource::SniffCodec(empty.Path()) == Codec::None);

    // Missing path: `file_size` returns an error; must not throw.
    const auto missing = std::filesystem::temp_directory_path() /
                         "slv-test-does-not-exist-8f4b3c2d1e5a.tmp";
    std::error_code ignoreEc;
    std::filesystem::remove(missing, ignoreEc);
    CHECK(DecompressingByteSource::SniffCodec(missing) == Codec::None);

    // Directory (POSIX file_size on a directory is an error; must not throw).
    CHECK(DecompressingByteSource::SniffCodec(std::filesystem::temp_directory_path()) == Codec::None);

    // Short-but-non-matching prefix: 1 byte, no codec magic starts
    // that short so this must classify as plain text.
    const TempBinaryFile tiny(".log");
    tiny.WriteBytes({0x7B}); // '{' -- not a magic prefix for anything we recognise
    CHECK(DecompressingByteSource::SniffCodec(tiny.Path()) == Codec::None);
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
