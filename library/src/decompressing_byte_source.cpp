#include "loglib/internal/decompressing_byte_source.hpp"

#include <fmt/format.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <ios>
#include <random>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

// Codec headers. All four codecs are linked PRIVATE to loglib; their
// types must not leak out of this TU. Everything below the include
// block stays inside `loglib::internal` and does not touch the public
// header's storage.
#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

namespace loglib::internal
{

namespace
{

/// Size of the streaming I/O buffers used for every codec. 64 KiB is
/// small enough to keep worker-thread RSS bounded on large files and
/// large enough to amortise per-chunk overhead (zlib's inflate benefits
/// from ≥16 KiB feeds).
constexpr std::size_t kChunkSize = 64 * 1024;

/// Bytes we sniff for the initial codec probe. Six is the widest
/// codec magic we recognise (`.xz` magic is 6 bytes).
constexpr std::size_t kMagicMaxBytes = 6;

/// Codec magic-byte prefixes. All four are extension-agnostic —
/// detection is content-first.
constexpr std::array<std::uint8_t, 2> kGzipMagic = {0x1f, 0x8b};
constexpr std::array<std::uint8_t, 3> kBzip2Magic = {'B', 'Z', 'h'};
constexpr std::array<std::uint8_t, 6> kXzMagic = {0xfd, '7', 'z', 'X', 'Z', 0x00};
constexpr std::array<std::uint8_t, 4> kZstdMagic = {0x28, 0xb5, 0x2f, 0xfd};

[[nodiscard]] bool StartsWith(std::span<const std::uint8_t> haystack, std::span<const std::uint8_t> needle) noexcept
{
    if (haystack.size() < needle.size())
    {
        return false;
    }
    return std::memcmp(haystack.data(), needle.data(), needle.size()) == 0;
}

/// Read up to @p max bytes from the beginning of @p path. On IO error
/// or missing file, throws `std::runtime_error`.
[[nodiscard]] std::vector<std::uint8_t> ReadMagic(const std::filesystem::path &path, std::size_t max)
{
    std::ifstream stream(path, std::ios::binary);
    if (!stream.is_open())
    {
        throw std::runtime_error(fmt::format("Failed to open '{}' for codec sniff", path.string()));
    }
    std::vector<std::uint8_t> out(max);
    stream.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(max));
    const auto got = static_cast<std::size_t>(stream.gcount());
    out.resize(got);
    return out;
}

[[nodiscard]] DecompressingByteSource::Codec DetectCodec(std::span<const std::uint8_t> magic) noexcept
{
    using Codec = DecompressingByteSource::Codec;
    if (StartsWith(magic, {kGzipMagic.data(), kGzipMagic.size()}))
    {
        return Codec::Gzip;
    }
    if (StartsWith(magic, {kBzip2Magic.data(), kBzip2Magic.size()}))
    {
        return Codec::Bzip2;
    }
    if (StartsWith(magic, {kXzMagic.data(), kXzMagic.size()}))
    {
        return Codec::Xz;
    }
    if (StartsWith(magic, {kZstdMagic.data(), kZstdMagic.size()}))
    {
        return Codec::Zstd;
    }
    return Codec::None;
}

/// Build a unique temp file path under `std::filesystem::temp_directory_path()`.
/// Uses a random suffix seeded from `std::random_device` + steady-clock
/// so parallel workers don't collide. The file is not created here —
/// the caller opens it exclusively.
[[nodiscard]] std::filesystem::path MakeTempPath()
{
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec)
    {
        throw std::runtime_error(fmt::format("temp_directory_path() failed: {}", ec.message()));
    }

    // Combine random_device and steady_clock so we don't rely on
    // random_device alone (deterministic on some platforms).
    static std::atomic<std::uint32_t> counter{0};
    std::random_device rd;
    const std::uint64_t r1 = (static_cast<std::uint64_t>(rd()) << 32) | rd();
    const std::uint64_t r2 = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count()
    );
    const std::uint32_t c = counter.fetch_add(1, std::memory_order_relaxed);

    return base / fmt::format("slv-decompressed-{:016x}{:016x}{:08x}.tmp", r1, r2, c);
}

/// Wrap a `std::FILE*` in RAII. We prefer `std::FILE*` over
/// `std::ofstream` for the temp file because it exposes buffered
/// `fwrite` with predictable failure surfaces and a native handle we
/// can flush at close.
struct FileHandle
{
    std::FILE *handle = nullptr;

    FileHandle() noexcept = default;
    explicit FileHandle(std::FILE *h) noexcept : handle(h)
    {
    }
    FileHandle(const FileHandle &) = delete;
    FileHandle &operator=(const FileHandle &) = delete;

    FileHandle(FileHandle &&other) noexcept : handle(other.handle)
    {
        other.handle = nullptr;
    }
    FileHandle &operator=(FileHandle &&other) noexcept
    {
        if (this != &other)
        {
            Close();
            handle = other.handle;
            other.handle = nullptr;
        }
        return *this;
    }
    ~FileHandle()
    {
        Close();
    }

    void Close() noexcept
    {
        if (handle != nullptr)
        {
            std::fclose(handle);
            handle = nullptr;
        }
    }
};

[[nodiscard]] FileHandle OpenExclusive(const std::filesystem::path &path)
{
#ifdef _WIN32
    std::FILE *raw = nullptr;
    // "wbN" -> write, binary, don't inherit. `_wfopen_s` handles wide paths.
    const errno_t err = ::_wfopen_s(&raw, path.native().c_str(), L"wbN");
    if (err != 0 || raw == nullptr)
    {
        throw std::runtime_error(
            fmt::format("Failed to create temp file '{}': errno {}", path.string(), err)
        );
    }
#else
    std::FILE *raw = std::fopen(path.string().c_str(), "wb");
    if (raw == nullptr)
    {
        const int savedErrno = errno;
        throw std::runtime_error(
            fmt::format("Failed to create temp file '{}': errno {}", path.string(), savedErrno)
        );
    }
#endif
    return FileHandle(raw);
}

void WriteAll(FileHandle &out, const void *data, std::size_t bytes, const std::filesystem::path &tempPath)
{
    if (bytes == 0)
    {
        return;
    }
    const std::size_t wrote = std::fwrite(data, 1, bytes, out.handle);
    if (wrote != bytes)
    {
        throw std::runtime_error(
            fmt::format("Short write to temp file '{}' ({} of {} bytes)", tempPath.string(), wrote, bytes)
        );
    }
}

/// Every decoder cooperates with the same StopToken poll + progress
/// callback fire pattern; we call this after each input chunk.
inline void ObservePoll(
    std::size_t bytesInSoFar,
    std::size_t totalBytesIn,
    const DecompressingByteSource::ProgressCallback &progress,
    const StopToken &stopToken
)
{
    if (progress)
    {
        progress({bytesInSoFar, totalBytesIn});
    }
    if (stopToken.stop_requested())
    {
        throw DecompressionCancelled("decompression cancelled by StopToken");
    }
}

// --- gzip / zlib -------------------------------------------------------

void DecodeGzip(
    std::ifstream &in,
    FileHandle &out,
    const std::filesystem::path &sourcePath,
    const std::filesystem::path &tempPath,
    std::size_t totalBytesIn,
    const DecompressingByteSource::ProgressCallback &progress,
    const StopToken &stopToken,
    std::size_t &decompressedSize
)
{
    z_stream strm{};
    // `windowBits = 15 + 32` accepts both zlib (RFC 1950) and gzip
    // (RFC 1952) streams, and auto-detects across concatenated members.
    if (::inflateInit2(&strm, 15 + 32) != Z_OK)
    {
        throw std::runtime_error(fmt::format("Failed to init zlib inflate for '{}'", sourcePath.string()));
    }
    struct ZlibGuard
    {
        z_stream *s;
        ~ZlibGuard()
        {
            ::inflateEnd(s);
        }
    } guard{&strm};

    std::array<Bytef, kChunkSize> inBuf{};
    std::array<Bytef, kChunkSize> outBuf{};

    std::size_t consumed = 0;
    // `ret` persists across read-chunk iterations: a member can end
    // exactly when `avail_in` hits 0 (member boundary aligned with the
    // 64 KiB read chunk), so the reset for the *next* concatenated
    // member is deferred to the iteration that reads fresh input.
    int ret = Z_OK;
    for (;;)
    {
        in.read(reinterpret_cast<char *>(inBuf.data()), static_cast<std::streamsize>(inBuf.size()));
        const auto gotSigned = in.gcount();
        if (gotSigned < 0)
        {
            throw std::runtime_error(fmt::format("Failed to read from '{}'", sourcePath.string()));
        }
        const bool eof = in.eof() || gotSigned == 0;
        strm.next_in = inBuf.data();
        strm.avail_in = static_cast<uInt>(gotSigned);
        consumed += static_cast<std::size_t>(gotSigned);
        ObservePoll(consumed, totalBytesIn, progress, stopToken);

        // A pure-EOF read (0 bytes) has nothing to decode; the final
        // validity check after the loop catches truncation.
        if (gotSigned == 0)
        {
            break;
        }

        // Drain this chunk. The loop keeps calling inflate while there
        // is buffered output to flush (`avail_out == 0`) or input left
        // to consume (`avail_in > 0`), which lets a single chunk
        // straddle a concatenated-member boundary and lets highly
        // compressible members drain even after their input is gone.
        do
        {
            if (ret == Z_STREAM_END)
            {
                if (strm.avail_in == 0)
                {
                    // Member finished exactly at the chunk edge; wait
                    // for the next read to learn if another follows.
                    break;
                }
                // Bytes remain after a member end -> concatenated
                // member. Reset before decoding them.
                if (::inflateReset(&strm) != Z_OK)
                {
                    throw std::runtime_error(
                        fmt::format("zlib inflateReset failed on '{}' at input byte {}", sourcePath.string(), consumed)
                    );
                }
                ret = Z_OK;
            }
            strm.next_out = outBuf.data();
            strm.avail_out = static_cast<uInt>(outBuf.size());
            // Use Z_NO_FLUSH so concatenated members work uniformly:
            // Z_FINISH combined with reset-on-Z_STREAM_END is fragile
            // when a member boundary lands mid-buffer.
            ret = ::inflate(&strm, Z_NO_FLUSH);
            switch (ret)
            {
            case Z_NEED_DICT:
            case Z_DATA_ERROR:
            case Z_MEM_ERROR:
            case Z_STREAM_ERROR:
                throw std::runtime_error(
                    fmt::format("zlib inflate error on '{}' at input byte {} (code {})", sourcePath.string(), consumed, ret)
                );
            default:
                break;
            }
            const std::size_t produced = outBuf.size() - strm.avail_out;
            WriteAll(out, outBuf.data(), produced, tempPath);
            decompressedSize += produced;
        } while (strm.avail_out == 0 || strm.avail_in > 0);

        if (eof)
        {
            break;
        }
    }

    // Reaching real EOF without a clean member end means truncation.
    if (ret != Z_STREAM_END)
    {
        throw std::runtime_error(
            fmt::format("Unexpected EOF in gzip stream '{}' at input byte {}", sourcePath.string(), consumed)
        );
    }
}

// --- bzip2 -------------------------------------------------------------

void DecodeBzip2(
    std::ifstream &in,
    FileHandle &out,
    const std::filesystem::path &sourcePath,
    const std::filesystem::path &tempPath,
    std::size_t totalBytesIn,
    const DecompressingByteSource::ProgressCallback &progress,
    const StopToken &stopToken,
    std::size_t &decompressedSize
)
{
    bz_stream strm{};
    if (::BZ2_bzDecompressInit(&strm, 0, 0) != BZ_OK)
    {
        throw std::runtime_error(fmt::format("Failed to init bzip2 for '{}'", sourcePath.string()));
    }
    struct BzGuard
    {
        bz_stream *s;
        ~BzGuard()
        {
            ::BZ2_bzDecompressEnd(s);
        }
    } guard{&strm};

    std::array<char, kChunkSize> inBuf{};
    std::array<char, kChunkSize> outBuf{};

    std::size_t consumed = 0;
    // See `DecodeGzip`: `ret` persists across read-chunk iterations so
    // a member that ends exactly at the chunk edge defers its reset to
    // the iteration that reads the next member's bytes.
    int ret = BZ_OK;
    for (;;)
    {
        in.read(inBuf.data(), static_cast<std::streamsize>(inBuf.size()));
        const auto gotSigned = in.gcount();
        if (gotSigned < 0)
        {
            throw std::runtime_error(fmt::format("Failed to read from '{}'", sourcePath.string()));
        }
        const bool eof = in.eof() || gotSigned == 0;
        strm.next_in = inBuf.data();
        strm.avail_in = static_cast<unsigned>(gotSigned);
        consumed += static_cast<std::size_t>(gotSigned);
        ObservePoll(consumed, totalBytesIn, progress, stopToken);

        if (gotSigned == 0)
        {
            break;
        }

        // Drain buffered output (`avail_out == 0`) and remaining input
        // (`avail_in > 0`) so a member boundary landing mid-buffer or
        // exactly at the chunk edge still feeds the next member.
        do
        {
            if (ret == BZ_STREAM_END)
            {
                if (strm.avail_in == 0)
                {
                    break;
                }
                // Concatenated bz2 stream: end + re-init for the next
                // member before decoding the remaining bytes.
                if (::BZ2_bzDecompressEnd(&strm) != BZ_OK || ::BZ2_bzDecompressInit(&strm, 0, 0) != BZ_OK)
                {
                    throw std::runtime_error(
                        fmt::format("bzip2 reset failed on '{}' at input byte {}", sourcePath.string(), consumed)
                    );
                }
                ret = BZ_OK;
            }
            strm.next_out = outBuf.data();
            strm.avail_out = static_cast<unsigned>(outBuf.size());
            ret = ::BZ2_bzDecompress(&strm);
            if (ret != BZ_OK && ret != BZ_STREAM_END)
            {
                throw std::runtime_error(
                    fmt::format("bzip2 decompress error on '{}' at input byte {} (code {})", sourcePath.string(), consumed, ret)
                );
            }
            const std::size_t produced = outBuf.size() - strm.avail_out;
            WriteAll(out, outBuf.data(), produced, tempPath);
            decompressedSize += produced;
        } while (strm.avail_out == 0 || strm.avail_in > 0);

        if (eof)
        {
            break;
        }
    }

    if (ret != BZ_STREAM_END)
    {
        throw std::runtime_error(
            fmt::format("Unexpected EOF in bzip2 stream '{}' at input byte {}", sourcePath.string(), consumed)
        );
    }
}

// --- xz / lzma ---------------------------------------------------------

void DecodeXz(
    std::ifstream &in,
    FileHandle &out,
    const std::filesystem::path &sourcePath,
    const std::filesystem::path &tempPath,
    std::size_t totalBytesIn,
    const DecompressingByteSource::ProgressCallback &progress,
    const StopToken &stopToken,
    std::size_t &decompressedSize
)
{
    lzma_stream strm = LZMA_STREAM_INIT;
    // `LZMA_CONCATENATED` lets us stream through concatenated members
    // (like `xzcat file1.xz file2.xz`).
    const lzma_ret initRet = ::lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED);
    if (initRet != LZMA_OK)
    {
        throw std::runtime_error(
            fmt::format("Failed to init xz decoder for '{}' (code {})", sourcePath.string(), static_cast<int>(initRet))
        );
    }
    struct XzGuard
    {
        lzma_stream *s;
        ~XzGuard()
        {
            ::lzma_end(s);
        }
    } guard{&strm};

    std::array<std::uint8_t, kChunkSize> inBuf{};
    std::array<std::uint8_t, kChunkSize> outBuf{};

    std::size_t consumed = 0;
    lzma_ret ret = LZMA_OK;
    lzma_action action = LZMA_RUN;
    while (ret != LZMA_STREAM_END)
    {
        if (strm.avail_in == 0 && action == LZMA_RUN)
        {
            in.read(reinterpret_cast<char *>(inBuf.data()), static_cast<std::streamsize>(inBuf.size()));
            const auto gotSigned = in.gcount();
            if (gotSigned < 0)
            {
                throw std::runtime_error(fmt::format("Failed to read from '{}'", sourcePath.string()));
            }
            strm.next_in = inBuf.data();
            strm.avail_in = static_cast<std::size_t>(gotSigned);
            if (in.eof() || gotSigned == 0)
            {
                action = LZMA_FINISH;
            }
            consumed += static_cast<std::size_t>(gotSigned);
            ObservePoll(consumed, totalBytesIn, progress, stopToken);
        }

        strm.next_out = outBuf.data();
        strm.avail_out = outBuf.size();
        ret = ::lzma_code(&strm, action);
        if (ret != LZMA_OK && ret != LZMA_STREAM_END)
        {
            throw std::runtime_error(
                fmt::format("xz decode error on '{}' at input byte {} (code {})", sourcePath.string(), consumed, static_cast<int>(ret))
            );
        }
        const std::size_t produced = outBuf.size() - strm.avail_out;
        WriteAll(out, outBuf.data(), produced, tempPath);
        decompressedSize += produced;
    }
}

// --- zstd --------------------------------------------------------------

void DecodeZstd(
    std::ifstream &in,
    FileHandle &out,
    const std::filesystem::path &sourcePath,
    const std::filesystem::path &tempPath,
    std::size_t totalBytesIn,
    const DecompressingByteSource::ProgressCallback &progress,
    const StopToken &stopToken,
    std::size_t &decompressedSize
)
{
    ZSTD_DCtx *dctx = ::ZSTD_createDCtx();
    if (dctx == nullptr)
    {
        throw std::runtime_error(fmt::format("Failed to create zstd DCtx for '{}'", sourcePath.string()));
    }
    struct ZstdGuard
    {
        ZSTD_DCtx *ctx;
        ~ZstdGuard()
        {
            ::ZSTD_freeDCtx(ctx);
        }
    } guard{dctx};

    std::array<char, kChunkSize> inBuf{};
    std::array<char, kChunkSize> outBuf{};

    std::size_t consumed = 0;
    std::size_t lastResult = 0;

    while (true)
    {
        in.read(inBuf.data(), static_cast<std::streamsize>(inBuf.size()));
        const auto gotSigned = in.gcount();
        if (gotSigned < 0)
        {
            throw std::runtime_error(fmt::format("Failed to read from '{}'", sourcePath.string()));
        }
        if (gotSigned == 0)
        {
            break;
        }

        ZSTD_inBuffer input{inBuf.data(), static_cast<std::size_t>(gotSigned), 0};
        while (input.pos < input.size)
        {
            ZSTD_outBuffer output{outBuf.data(), outBuf.size(), 0};
            const std::size_t result = ::ZSTD_decompressStream(dctx, &output, &input);
            if (::ZSTD_isError(result) != 0U)
            {
                throw std::runtime_error(fmt::format(
                    "zstd decode error on '{}' at input byte {} ({})",
                    sourcePath.string(),
                    consumed + input.pos,
                    ::ZSTD_getErrorName(result)
                ));
            }
            WriteAll(out, output.dst, output.pos, tempPath);
            decompressedSize += output.pos;
            lastResult = result;
        }

        consumed += static_cast<std::size_t>(gotSigned);
        ObservePoll(consumed, totalBytesIn, progress, stopToken);

        if (in.eof())
        {
            break;
        }
    }
    if (lastResult != 0)
    {
        // Non-zero return value at EOF means the last frame wasn't
        // fully consumed (truncated input).
        throw std::runtime_error(
            fmt::format("Unexpected EOF in zstd stream '{}' at input byte {}", sourcePath.string(), consumed)
        );
    }
}

} // namespace

DecompressingByteSource::DecompressingByteSource(
    std::filesystem::path input, ProgressCallback progress, StopToken stopToken
)
    : mDisplayPath(std::move(input))
    , mEffectivePath(mDisplayPath)
{
    std::error_code ec;
    // MSVC's <filesystem> flag-cast trips clang-analyzer's enum-cast check.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    if (!std::filesystem::exists(mDisplayPath, ec) || ec)
    {
        throw std::runtime_error(fmt::format("File '{}' does not exist", mDisplayPath.string()));
    }
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto size = std::filesystem::file_size(mDisplayPath, ec);
    if (ec)
    {
        throw std::runtime_error(
            fmt::format("Failed to stat '{}': {}", mDisplayPath.string(), ec.message())
        );
    }
    mCompressedSize = static_cast<std::size_t>(size);

    if (mCompressedSize == 0)
    {
        // Empty file: nothing to sniff, nothing to decompress.
        return;
    }

    const std::vector<std::uint8_t> magic = ReadMagic(mDisplayPath, kMagicMaxBytes);
    mCodec = DetectCodec({magic.data(), magic.size()});
    if (mCodec == Codec::None)
    {
        // Not a codec we recognise — passthrough.
        return;
    }

    // Compressed: stream-decode into a temp file. Any exception from
    // the codec must delete the partial temp file before propagating.
    // Note: `mOwnsTempFile` is deliberately deferred until
    // `OpenExclusive` succeeds -- before that point no file exists
    // on disk, so an early throw (from opening the *input* stream)
    // must NOT trigger `ReleaseTempFile()`. Setting the flag inside
    // the try scope keeps the "owns the file" invariant tight.
    const std::filesystem::path tempPath = MakeTempPath();
    mEffectivePath = tempPath;

    std::ifstream inStream(mDisplayPath, std::ios::binary);
    if (!inStream.is_open())
    {
        // Undo the temp path assignment before throwing.
        mEffectivePath = mDisplayPath;
        throw std::runtime_error(fmt::format("Failed to open '{}' for decompression", mDisplayPath.string()));
    }

    try
    {
        FileHandle outHandle = OpenExclusive(tempPath);
        // Only now does a file exist on disk that we're responsible
        // for cleaning up. Any exception below unwinds through the
        // catch block, which calls ReleaseTempFile().
        mOwnsTempFile = true;

        switch (mCodec)
        {
        case Codec::Gzip:
            DecodeGzip(inStream, outHandle, mDisplayPath, tempPath, mCompressedSize, progress, stopToken, mDecompressedSize);
            break;
        case Codec::Bzip2:
            DecodeBzip2(inStream, outHandle, mDisplayPath, tempPath, mCompressedSize, progress, stopToken, mDecompressedSize);
            break;
        case Codec::Xz:
            DecodeXz(inStream, outHandle, mDisplayPath, tempPath, mCompressedSize, progress, stopToken, mDecompressedSize);
            break;
        case Codec::Zstd:
            DecodeZstd(inStream, outHandle, mDisplayPath, tempPath, mCompressedSize, progress, stopToken, mDecompressedSize);
            break;
        case Codec::None:
            // Handled above; the switch enumerates for -Wswitch.
            break;
        }
        // Explicit flush before close so a partial-write failure is
        // visible here rather than at destructor time.
        if (std::fflush(outHandle.handle) != 0)
        {
            const int savedErrno = errno;
            throw std::runtime_error(
                fmt::format("Failed to flush temp file '{}': errno {}", tempPath.string(), savedErrno)
            );
        }
    }
    catch (...)
    {
        ReleaseTempFile();
        throw;
    }
}

DecompressingByteSource::~DecompressingByteSource()
{
    ReleaseTempFile();
}

DecompressingByteSource::DecompressingByteSource(DecompressingByteSource &&other) noexcept
    : mDisplayPath(std::move(other.mDisplayPath))
    , mEffectivePath(std::move(other.mEffectivePath))
    , mCodec(other.mCodec)
    , mCompressedSize(other.mCompressedSize)
    , mDecompressedSize(other.mDecompressedSize)
    , mOwnsTempFile(other.mOwnsTempFile)
{
    other.mCodec = Codec::None;
    other.mCompressedSize = 0;
    other.mDecompressedSize = 0;
    other.mOwnsTempFile = false;
}

DecompressingByteSource &DecompressingByteSource::operator=(DecompressingByteSource &&other) noexcept
{
    if (this != &other)
    {
        ReleaseTempFile();
        mDisplayPath = std::move(other.mDisplayPath);
        mEffectivePath = std::move(other.mEffectivePath);
        mCodec = other.mCodec;
        mCompressedSize = other.mCompressedSize;
        mDecompressedSize = other.mDecompressedSize;
        mOwnsTempFile = other.mOwnsTempFile;
        other.mCodec = Codec::None;
        other.mCompressedSize = 0;
        other.mDecompressedSize = 0;
        other.mOwnsTempFile = false;
    }
    return *this;
}

void DecompressingByteSource::ReleaseTempFile() noexcept
{
    if (mOwnsTempFile && !mEffectivePath.empty())
    {
        std::error_code ec;
        std::filesystem::remove(mEffectivePath, ec);
        // Best-effort: a leaked temp is a footprint problem, not a
        // correctness problem. Silence any error.
    }
    mOwnsTempFile = false;
}

const std::filesystem::path &DecompressingByteSource::DisplayPath() const noexcept
{
    return mDisplayPath;
}

const std::filesystem::path &DecompressingByteSource::EffectivePath() const noexcept
{
    return mEffectivePath;
}

bool DecompressingByteSource::WasDecompressed() const noexcept
{
    return mCodec != Codec::None;
}

DecompressingByteSource::Codec DecompressingByteSource::DetectedCodec() const noexcept
{
    return mCodec;
}

std::size_t DecompressingByteSource::CompressedSize() const noexcept
{
    return mCompressedSize;
}

std::size_t DecompressingByteSource::DecompressedSize() const noexcept
{
    return mDecompressedSize;
}

std::string_view CodecName(DecompressingByteSource::Codec codec) noexcept
{
    using Codec = DecompressingByteSource::Codec;
    switch (codec)
    {
    case Codec::None:
        return "none";
    case Codec::Gzip:
        return "gzip";
    case Codec::Bzip2:
        return "bzip2";
    case Codec::Xz:
        return "xz";
    case Codec::Zstd:
        return "zstd";
    }
    return "unknown";
}

} // namespace loglib::internal
