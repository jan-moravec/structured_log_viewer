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

// POSIX-only headers for exclusive-create; MSVC lacks these.
#ifndef _WIN32
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#endif

// Codec headers. Linked PRIVATE to loglib; keep their types confined
// to this TU (do not surface them from the public header).
#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>
#include <zstd.h>

namespace loglib::internal
{

namespace
{

/// Shared 64 KiB codec buffer size.
constexpr std::size_t CHUNK_SIZE = 64 * 1024;

/// Bytes read for the codec probe. Matches the widest supported
/// magic (`.xz` is 6 bytes).
constexpr std::size_t MAGIC_MAX_BYTES = 6;

/// Extension-independent codec signatures.
constexpr std::array<std::uint8_t, 2> GZIP_MAGIC = {0x1f, 0x8b};
constexpr std::array<std::uint8_t, 3> BZIP2_MAGIC = {'B', 'Z', 'h'};
constexpr std::array<std::uint8_t, 6> XZ_MAGIC = {0xfd, '7', 'z', 'X', 'Z', 0x00};
constexpr std::array<std::uint8_t, 4> ZSTD_MAGIC = {0x28, 0xb5, 0x2f, 0xfd};

/// Zstd skippable-frame magic range from RFC 8878 §3.1.2.
constexpr std::uint8_t ZSTD_SKIPPABLE_MAGIC_BYTE0_MASK = 0xF0;
constexpr std::uint8_t ZSTD_SKIPPABLE_MAGIC_BYTE0_VALUE = 0x50;
constexpr std::uint8_t ZSTD_SKIPPABLE_MAGIC_BYTE1 = 0x2A;
constexpr std::uint8_t ZSTD_SKIPPABLE_MAGIC_BYTE2 = 0x4D;
constexpr std::uint8_t ZSTD_SKIPPABLE_MAGIC_BYTE3 = 0x18;

/// Return whether bytes start with a zstd skippable frame.
[[nodiscard]] bool IsZstdSkippableMagic(std::span<const std::uint8_t> haystack) noexcept
{
    return haystack.size() >= 4 &&
           (haystack[0] & ZSTD_SKIPPABLE_MAGIC_BYTE0_MASK) == ZSTD_SKIPPABLE_MAGIC_BYTE0_VALUE &&
           haystack[1] == ZSTD_SKIPPABLE_MAGIC_BYTE1 && haystack[2] == ZSTD_SKIPPABLE_MAGIC_BYTE2 &&
           haystack[3] == ZSTD_SKIPPABLE_MAGIC_BYTE3;
}

[[nodiscard]] bool StartsWith(std::span<const std::uint8_t> haystack, std::span<const std::uint8_t> needle) noexcept
{
    if (haystack.size() < needle.size())
    {
        return false;
    }
    return std::memcmp(haystack.data(), needle.data(), needle.size()) == 0;
}

/// Read up to @p max leading bytes, throwing on open failure.
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

/// Non-throwing `ReadMagic`; returns empty on I/O failure.
[[nodiscard]] std::vector<std::uint8_t> TryReadMagic(const std::filesystem::path &path, std::size_t max) noexcept
{
    try
    {
        std::ifstream stream(path, std::ios::binary);
        if (!stream.is_open())
        {
            return {};
        }
        std::vector<std::uint8_t> out(max);
        stream.read(reinterpret_cast<char *>(out.data()), static_cast<std::streamsize>(max));
        const auto got = static_cast<std::size_t>(stream.gcount());
        out.resize(got);
        return out;
    }
    catch (...)
    {
        return {};
    }
}

[[nodiscard]] DecompressingByteSource::Codec DetectCodec(std::span<const std::uint8_t> magic) noexcept
{
    using Codec = DecompressingByteSource::Codec;
    if (StartsWith(magic, {GZIP_MAGIC.data(), GZIP_MAGIC.size()}))
    {
        return Codec::Gzip;
    }
    if (StartsWith(magic, {BZIP2_MAGIC.data(), BZIP2_MAGIC.size()}))
    {
        return Codec::Bzip2;
    }
    if (StartsWith(magic, {XZ_MAGIC.data(), XZ_MAGIC.size()}))
    {
        return Codec::Xz;
    }
    if (StartsWith(magic, {ZSTD_MAGIC.data(), ZSTD_MAGIC.size()}) || IsZstdSkippableMagic(magic))
    {
        return Codec::Zstd;
    }
    return Codec::None;
}

/// Build a unique candidate path in the system temp directory.
[[nodiscard]] std::filesystem::path MakeTempPath()
{
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec)
    {
        throw std::runtime_error(fmt::format("temp_directory_path() failed: {}", ec.message()));
    }

    // Mix random_device with steady_clock -- random_device alone
    // is deterministic on some platforms.
    static std::atomic<std::uint32_t> counter{0};
    std::random_device rd;
    const std::uint64_t r1 = (static_cast<std::uint64_t>(rd()) << 32) | rd();
    const std::uint64_t r2 = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::uint32_t c = counter.fetch_add(1, std::memory_order_relaxed);

    return base / fmt::format("slv-decompressed-{:016x}{:016x}{:08x}.tmp", r1, r2, c);
}

/// RAII wrapper around `std::FILE*`.
class FileHandle
{
public:
    FileHandle() noexcept = default;
    explicit FileHandle(std::FILE *h) noexcept
        : mHandle(h)
    {
    }
    FileHandle(const FileHandle &) = delete;
    FileHandle &operator=(const FileHandle &) = delete;

    FileHandle(FileHandle &&other) noexcept
        : mHandle(other.mHandle)
    {
        other.mHandle = nullptr;
    }
    FileHandle &operator=(FileHandle &&other) noexcept
    {
        if (this != &other)
        {
            Close();
            mHandle = other.mHandle;
            other.mHandle = nullptr;
        }
        return *this;
    }
    ~FileHandle()
    {
        Close();
    }

    [[nodiscard]] std::FILE *Get() const noexcept
    {
        return mHandle;
    }
    [[nodiscard]] bool IsOpen() const noexcept
    {
        return mHandle != nullptr;
    }

    void Close() noexcept
    {
        if (mHandle != nullptr)
        {
            // NOLINTNEXTLINE(cppcoreguidelines-owning-memory,cert-err33-c)
            (void)std::fclose(mHandle);
            mHandle = nullptr;
        }
    }

private:
    std::FILE *mHandle = nullptr;
};

/// Collision error returned by exclusive temp-file creation.
constexpr int TEMP_FILE_COLLISION_ERRNO = EEXIST;

/// Result of a single `TryOpenExclusive` attempt.
struct OpenExclusiveResult
{
    FileHandle handle;
    bool collided = false;
    int savedErrno = 0;
};

[[nodiscard]] OpenExclusiveResult TryOpenExclusive(const std::filesystem::path &path) noexcept
{
#ifdef _WIN32
    std::FILE *raw = nullptr;
    const errno_t err = ::_wfopen_s(&raw, path.native().c_str(), L"wbxN");
    if (err != 0 || raw == nullptr)
    {
        return OpenExclusiveResult{
            .handle = FileHandle(), .collided = err == TEMP_FILE_COLLISION_ERRNO, .savedErrno = err
        };
    }
    return OpenExclusiveResult{.handle = FileHandle(raw), .collided = false, .savedErrno = 0};
#else
    // O_CLOEXEC is POSIX.1-2008 and available on every supported
    // platform; still guarded in case a minimal platform lacks it.
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    // Use native path bytes and owner-only permissions.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,cppcoreguidelines-pro-type-cstyle-cast,hicpp-vararg)
    const int fd = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (fd < 0)
    {
        const int savedErrno = errno;
        return OpenExclusiveResult{
            .handle = FileHandle(), .collided = savedErrno == TEMP_FILE_COLLISION_ERRNO, .savedErrno = savedErrno
        };
    }
    std::FILE *raw = ::fdopen(fd, "wb");
    if (raw == nullptr)
    {
        const int savedErrno = errno;
        // Clean up the descriptor and newly created file.
        ::close(fd);
        std::error_code ignoreEc;
        std::filesystem::remove(path, ignoreEc);
        return OpenExclusiveResult{.handle = FileHandle(), .collided = false, .savedErrno = savedErrno};
    }
    return OpenExclusiveResult{.handle = FileHandle(raw), .collided = false, .savedErrno = 0};
#endif
}

/// Create a unique temp file, retrying bounded name collisions.
[[nodiscard]] FileHandle OpenExclusiveWithRetry(std::filesystem::path &outPath)
{
    constexpr int MAX_ATTEMPTS = 8;
    int lastErrno = 0;
    std::filesystem::path lastPath;
    for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt)
    {
        std::filesystem::path candidate = MakeTempPath();
        // Ownership moves on success; otherwise `result` closes it.
        // NOLINTNEXTLINE(clang-analyzer-unix.Stream)
        OpenExclusiveResult result = TryOpenExclusive(candidate);
        if (result.handle.IsOpen())
        {
            outPath = std::move(candidate);
            return std::move(result.handle);
        }
        lastErrno = result.savedErrno;
        lastPath = std::move(candidate);
        if (!result.collided)
        {
            break;
        }
    }
    throw std::runtime_error(fmt::format("Failed to create temp file '{}': errno {}", lastPath.string(), lastErrno));
}

void WriteAll(FileHandle &out, const void *data, std::size_t bytes, const std::filesystem::path &tempPath)
{
    if (bytes == 0)
    {
        return;
    }
    const std::size_t wrote = std::fwrite(data, 1, bytes, out.Get());
    if (wrote != bytes)
    {
        throw std::runtime_error(
            fmt::format("Short write to temp file '{}' ({} of {} bytes)", tempPath.string(), wrote, bytes)
        );
    }
}

/// Write output, update its size, and enforce the configured cap.
void WriteOutput(
    FileHandle &out,
    const void *data,
    std::size_t bytes,
    const std::filesystem::path &tempPath,
    const std::filesystem::path &sourcePath,
    std::size_t &decompressedSize,
    std::size_t maxDecompressedBytes
)
{
    WriteAll(out, data, bytes, tempPath);
    decompressedSize += bytes;
    if (maxDecompressedBytes != 0 && decompressedSize > maxDecompressedBytes)
    {
        throw DecompressionSizeCapExceeded(
            fmt::format(
                "Decompressed output for '{}' exceeded the {}-byte cap "
                "(would need at least {} bytes)",
                sourcePath.string(),
                maxDecompressedBytes,
                decompressedSize
            )
        );
    }
}

/// Shared progress-fire + stop-token poll used by every codec.
inline void ObservePoll(
    std::size_t bytesInSoFar,
    std::size_t totalBytesIn,
    const DecompressingByteSource::ProgressCallback &progress,
    const StopToken &stopToken
)
{
    if (progress)
    {
        progress({.bytesIn = bytesInSoFar, .totalBytesIn = totalBytesIn});
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
    std::size_t &decompressedSize,
    std::size_t maxDecompressedBytes
)
{
    z_stream strm{};
    // windowBits = 15 + 32: accept both zlib (RFC 1950) and gzip
    // (RFC 1952), auto-detect across concatenated members.
    if (::inflateInit2(&strm, 15 + 32) != Z_OK)
    {
        throw std::runtime_error(fmt::format("Failed to init zlib inflate for '{}'", sourcePath.string()));
    }
    class ZlibGuard
    {
    public:
        explicit ZlibGuard(z_stream *s) noexcept
            : mStream(s)
        {
        }
        ~ZlibGuard() noexcept
        {
            (void)::inflateEnd(mStream);
        }
        ZlibGuard(const ZlibGuard &) = delete;
        ZlibGuard &operator=(const ZlibGuard &) = delete;
        ZlibGuard(ZlibGuard &&) = delete;
        ZlibGuard &operator=(ZlibGuard &&) = delete;

    private:
        z_stream *mStream;
    };
    const ZlibGuard guard(&strm);

    std::array<Bytef, CHUNK_SIZE> inBuf{};
    std::array<Bytef, CHUNK_SIZE> outBuf{};

    std::size_t consumed = 0;
    // Preserve end-of-member state across chunk boundaries.
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

        // Pure-EOF read has nothing to decode; the post-loop check
        // catches truncation.
        if (gotSigned == 0)
        {
            break;
        }

        // Drain all input/output and poll between inflate calls.
        while (true)
        {
            if (ret == Z_STREAM_END)
            {
                if (strm.avail_in == 0)
                {
                    // Member ended at the chunk edge; wait for the
                    // next read to see if another follows.
                    break;
                }
                // Reset before the next concatenated member.
                if (::inflateReset(&strm) != Z_OK)
                {
                    throw std::runtime_error(
                        fmt::format("zlib inflateReset failed on '{}' at input byte {}", sourcePath.string(), consumed)
                    );
                }
            }
            ObservePoll(consumed, totalBytesIn, progress, stopToken);
            strm.next_out = outBuf.data();
            strm.avail_out = static_cast<uInt>(outBuf.size());
            // Z_NO_FLUSH handles concatenated members uniformly;
            // Z_FINISH + reset-on-Z_STREAM_END breaks when member
            // boundaries land mid-buffer.
            ret = ::inflate(&strm, Z_NO_FLUSH);
            switch (ret)
            {
            case Z_NEED_DICT:
            case Z_DATA_ERROR:
            case Z_MEM_ERROR:
            case Z_STREAM_ERROR:
                throw std::runtime_error(
                    fmt::format(
                        "zlib inflate error on '{}' at input byte {} (code {})", sourcePath.string(), consumed, ret
                    )
                );
            default:
                break;
            }
            const std::size_t produced = outBuf.size() - strm.avail_out;
            WriteOutput(out, outBuf.data(), produced, tempPath, sourcePath, decompressedSize, maxDecompressedBytes);
            if (strm.avail_out != 0 && strm.avail_in == 0)
            {
                break;
            }
        }

        if (eof)
        {
            break;
        }
    }

    // Real EOF without a clean member end == truncation.
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
    std::size_t &decompressedSize,
    std::size_t maxDecompressedBytes
)
{
    bz_stream strm{};
    if (::BZ2_bzDecompressInit(&strm, 0, 0) != BZ_OK)
    {
        throw std::runtime_error(fmt::format("Failed to init bzip2 for '{}'", sourcePath.string()));
    }
    class BzGuard
    {
    public:
        explicit BzGuard(bz_stream *s) noexcept
            : mStream(s)
        {
        }
        ~BzGuard() noexcept
        {
            (void)::BZ2_bzDecompressEnd(mStream);
        }
        BzGuard(const BzGuard &) = delete;
        BzGuard &operator=(const BzGuard &) = delete;
        BzGuard(BzGuard &&) = delete;
        BzGuard &operator=(BzGuard &&) = delete;

    private:
        bz_stream *mStream;
    };
    const BzGuard guard(&strm);

    std::array<char, CHUNK_SIZE> inBuf{};
    std::array<char, CHUNK_SIZE> outBuf{};

    std::size_t consumed = 0;
    // Preserve end-of-member state across chunk boundaries.
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

        // Drain all input/output and poll between decoder calls.
        while (true)
        {
            if (ret == BZ_STREAM_END)
            {
                if (strm.avail_in == 0)
                {
                    break;
                }
                // Reinitialize for the next concatenated member.
                if (::BZ2_bzDecompressEnd(&strm) != BZ_OK || ::BZ2_bzDecompressInit(&strm, 0, 0) != BZ_OK)
                {
                    throw std::runtime_error(
                        fmt::format("bzip2 reset failed on '{}' at input byte {}", sourcePath.string(), consumed)
                    );
                }
            }
            ObservePoll(consumed, totalBytesIn, progress, stopToken);
            strm.next_out = outBuf.data();
            strm.avail_out = static_cast<unsigned>(outBuf.size());
            ret = ::BZ2_bzDecompress(&strm);
            if (ret != BZ_OK && ret != BZ_STREAM_END)
            {
                throw std::runtime_error(
                    fmt::format(
                        "bzip2 decompress error on '{}' at input byte {} (code {})", sourcePath.string(), consumed, ret
                    )
                );
            }
            const std::size_t produced = outBuf.size() - strm.avail_out;
            WriteOutput(out, outBuf.data(), produced, tempPath, sourcePath, decompressedSize, maxDecompressedBytes);
            if (strm.avail_out != 0 && strm.avail_in == 0)
            {
                break;
            }
        }

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

// Use liblzma's multi-threaded decoder; single-block streams fall
// back to one worker.
void DecodeXz(
    std::ifstream &in,
    FileHandle &out,
    const std::filesystem::path &sourcePath,
    const std::filesystem::path &tempPath,
    std::size_t totalBytesIn,
    const DecompressingByteSource::ProgressCallback &progress,
    const StopToken &stopToken,
    std::size_t &decompressedSize,
    std::size_t maxDecompressedBytes
)
{
    lzma_stream strm = LZMA_STREAM_INIT;

    // Unknown CPU count falls back to one worker.
    std::uint32_t threads = ::lzma_cputhreads();
    if (threads == 0)
    {
        threads = 1;
    }

    lzma_mt mt = {};
    // Stream through concatenated members like the ST decoder does.
    mt.flags = LZMA_CONCATENATED;
    mt.threads = threads;
    // Bound cancellation latency while liblzma is idle.
    constexpr std::uint32_t XZ_MT_POLL_WINDOW_MS = 100;
    mt.timeout = XZ_MT_POLL_WINDOW_MS;
    // Hard cap: never fail with `LZMA_MEMLIMIT_ERROR`. Any file the
    // ST decoder accepts must still open here.
    mt.memlimit_stop = UINT64_MAX;
    // Limit threading memory to 25% of RAM, or 512 MiB if unknown.
    constexpr std::uint64_t XZ_MEMLIMIT_FALLBACK_MIB = 512;
    constexpr unsigned MIB_TO_BYTES_SHIFT = 20;
    constexpr unsigned PHYSMEM_FRACTION_SHIFT = 2; // physmem / 4
    const std::uint64_t physmem = ::lzma_physmem();
    mt.memlimit_threading =
        physmem == 0 ? (XZ_MEMLIMIT_FALLBACK_MIB << MIB_TO_BYTES_SHIFT) : (physmem >> PHYSMEM_FRACTION_SHIFT);

    const lzma_ret initRet = ::lzma_stream_decoder_mt(&strm, &mt);
    if (initRet != LZMA_OK)
    {
        throw std::runtime_error(
            fmt::format("Failed to init xz decoder for '{}' (code {})", sourcePath.string(), static_cast<int>(initRet))
        );
    }
    class XzGuard
    {
    public:
        explicit XzGuard(lzma_stream *s) noexcept
            : mStream(s)
        {
        }
        ~XzGuard() noexcept
        {
            ::lzma_end(mStream);
        }
        XzGuard(const XzGuard &) = delete;
        XzGuard &operator=(const XzGuard &) = delete;
        XzGuard(XzGuard &&) = delete;
        XzGuard &operator=(XzGuard &&) = delete;

    private:
        lzma_stream *mStream;
    };
    const XzGuard guard(&strm);

    std::array<std::uint8_t, CHUNK_SIZE> inBuf{};
    std::array<std::uint8_t, CHUNK_SIZE> outBuf{};

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
        }

        // Poll between decoder calls, not only between input reads.
        ObservePoll(consumed, totalBytesIn, progress, stopToken);

        strm.next_out = outBuf.data();
        strm.avail_out = outBuf.size();
        ret = ::lzma_code(&strm, action);
        if (ret != LZMA_OK && ret != LZMA_STREAM_END)
        {
            throw std::runtime_error(
                fmt::format(
                    "xz decode error on '{}' at input byte {} (code {})",
                    sourcePath.string(),
                    consumed,
                    static_cast<int>(ret)
                )
            );
        }
        const std::size_t produced = outBuf.size() - strm.avail_out;
        WriteOutput(out, outBuf.data(), produced, tempPath, sourcePath, decompressedSize, maxDecompressedBytes);
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
    std::size_t &decompressedSize,
    std::size_t maxDecompressedBytes,
    bool discardFirstLine,
    std::size_t maxDiscardedFirstLineBytes,
    std::string &discardedFirstLine
)
{
    ZSTD_DCtx *dctx = ::ZSTD_createDCtx();
    if (dctx == nullptr)
    {
        throw std::runtime_error(fmt::format("Failed to create zstd DCtx for '{}'", sourcePath.string()));
    }
    class ZstdGuard
    {
    public:
        explicit ZstdGuard(ZSTD_DCtx *ctx) noexcept
            : mCtx(ctx)
        {
        }
        ~ZstdGuard() noexcept
        {
            (void)::ZSTD_freeDCtx(mCtx);
        }
        ZstdGuard(const ZstdGuard &) = delete;
        ZstdGuard &operator=(const ZstdGuard &) = delete;
        ZstdGuard(ZstdGuard &&) = delete;
        ZstdGuard &operator=(ZstdGuard &&) = delete;

    private:
        ZSTD_DCtx *mCtx;
    };
    const ZstdGuard guard(dctx);

    std::array<char, CHUNK_SIZE> inBuf{};
    std::array<char, CHUNK_SIZE> outBuf{};

    std::size_t consumed = 0;
    std::size_t lastResult = 0;
    bool firstLineComplete = !discardFirstLine;

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

        consumed += static_cast<std::size_t>(gotSigned);

        ZSTD_inBuffer input{.src = inBuf.data(), .size = static_cast<std::size_t>(gotSigned), .pos = 0};
        while (input.pos < input.size)
        {
            // Poll between decoder calls for highly compressed frames.
            ObservePoll(consumed, totalBytesIn, progress, stopToken);
            ZSTD_outBuffer output{.dst = outBuf.data(), .size = outBuf.size(), .pos = 0};
            const std::size_t result = ::ZSTD_decompressStream(dctx, &output, &input);
            if (::ZSTD_isError(result) != 0U)
            {
                throw std::runtime_error(
                    fmt::format(
                        "zstd decode error on '{}' at input byte {} ({})",
                        sourcePath.string(),
                        (consumed - static_cast<std::size_t>(gotSigned)) + input.pos,
                        ::ZSTD_getErrorName(result)
                    )
                );
            }
            std::string_view produced(static_cast<const char *>(output.dst), output.pos);
            if (!firstLineComplete)
            {
                const std::size_t newline = produced.find('\n');
                const std::size_t prefixSize = newline == std::string_view::npos ? produced.size() : newline;
                if (discardedFirstLine.size() + prefixSize > maxDiscardedFirstLineBytes)
                {
                    throw std::runtime_error(
                        fmt::format("First decompressed line in '{}' exceeds metadata limit", sourcePath.string())
                    );
                }
                discardedFirstLine.append(produced.substr(0, prefixSize));
                if (newline != std::string_view::npos)
                {
                    firstLineComplete = true;
                    produced.remove_prefix(newline + 1);
                }
                else
                {
                    produced = {};
                }
            }
            WriteOutput(
                out, produced.data(), produced.size(), tempPath, sourcePath, decompressedSize, maxDecompressedBytes
            );
            lastResult = result;
        }

        if (in.eof())
        {
            break;
        }
    }
    if (lastResult != 0)
    {
        // Non-zero result at EOF -> truncated input (last frame
        // not fully consumed).
        throw std::runtime_error(
            fmt::format("Unexpected EOF in zstd stream '{}' at input byte {}", sourcePath.string(), consumed)
        );
    }
    if (!firstLineComplete)
    {
        throw std::runtime_error(
            fmt::format("Compressed stream '{}' has no metadata line terminator", sourcePath.string())
        );
    }
}

} // namespace

DecompressingByteSource::DecompressingByteSource(
    std::filesystem::path input, const ProgressCallback &progress, const StopToken &stopToken
)
    : DecompressingByteSource(std::move(input), progress, stopToken, Options{})
{
}

DecompressingByteSource::DecompressingByteSource(
    std::filesystem::path input, const ProgressCallback &progress, const StopToken &stopToken, Options options
)
    : mDisplayPath(std::move(input)), mEffectivePath(mDisplayPath)
{
    const std::size_t maxDecompressedBytes = options.maxDecompressedBytes;
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
        throw std::runtime_error(fmt::format("Failed to stat '{}': {}", mDisplayPath.string(), ec.message()));
    }
    mCompressedSize = static_cast<std::size_t>(size);

    if (mCompressedSize == 0)
    {
        // Empty file: passthrough. `discardFirstLine` has no line
        // to consume, so fail loudly (likely a truncated bundle)
        // instead of silently returning an empty first line.
        if (options.discardFirstLine)
        {
            throw std::invalid_argument(
                fmt::format("Cannot discard first line from empty file '{}'", mDisplayPath.string())
            );
        }
        return;
    }

    const std::vector<std::uint8_t> magic = ReadMagic(mDisplayPath, MAGIC_MAX_BYTES);
    mCodec = DetectCodec({magic.data(), magic.size()});
    if (mCodec == Codec::None)
    {
        // Uncompressed passthrough. Silently ignoring the discard
        // would leave the metadata line embedded in the returned
        // file, so a plain `.jsonl` opened through the bundle path
        // would parse it as row 1.
        if (options.discardFirstLine)
        {
            throw std::invalid_argument(
                fmt::format(
                    "discardFirstLine requires a compressed input; '{}' is not compressed", mDisplayPath.string()
                )
            );
        }
        return;
    }
    if (options.discardFirstLine && mCodec != Codec::Zstd)
    {
        // Only the zstd decode path plumbs `discardFirstLine`; fail
        // loudly rather than silently leaving the metadata line in
        // the decoded output.
        throw std::invalid_argument(
            fmt::format(
                "discardFirstLine is only supported for zstd streams; '{}' uses {}",
                mDisplayPath.string(),
                CodecName(mCodec)
            )
        );
    }

    // Decode to an owned temp file and remove it on failure.
    std::ifstream inStream(mDisplayPath, std::ios::binary);
    if (!inStream.is_open())
    {
        throw std::runtime_error(fmt::format("Failed to open '{}' for decompression", mDisplayPath.string()));
    }

    try
    {
        std::filesystem::path tempPath;
        FileHandle outHandle = OpenExclusiveWithRetry(tempPath);
        // From here on we own a file on disk; the catch below
        // unwinds via `ReleaseTempFile()`.
        mEffectivePath = tempPath;
        mOwnsTempFile = true;

        switch (mCodec)
        {
        case Codec::Gzip:
            DecodeGzip(
                inStream,
                outHandle,
                mDisplayPath,
                tempPath,
                mCompressedSize,
                progress,
                stopToken,
                mDecompressedSize,
                maxDecompressedBytes
            );
            break;
        case Codec::Bzip2:
            DecodeBzip2(
                inStream,
                outHandle,
                mDisplayPath,
                tempPath,
                mCompressedSize,
                progress,
                stopToken,
                mDecompressedSize,
                maxDecompressedBytes
            );
            break;
        case Codec::Xz:
            DecodeXz(
                inStream,
                outHandle,
                mDisplayPath,
                tempPath,
                mCompressedSize,
                progress,
                stopToken,
                mDecompressedSize,
                maxDecompressedBytes
            );
            break;
        case Codec::Zstd:
            DecodeZstd(
                inStream,
                outHandle,
                mDisplayPath,
                tempPath,
                mCompressedSize,
                progress,
                stopToken,
                mDecompressedSize,
                maxDecompressedBytes,
                options.discardFirstLine,
                options.maxDiscardedFirstLineBytes,
                mDiscardedFirstLine
            );
            break;
        case Codec::None:
            // Handled above; enumerated here for -Wswitch.
            break;
        }
        // Explicit flush so partial-write failures surface here
        // rather than at destructor time.
        if (std::fflush(outHandle.Get()) != 0)
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
        // Restore effective path so a caller inspecting it for
        // diagnostics doesn't see the just-unlinked temp file.
        mEffectivePath = mDisplayPath;
        throw;
    }
}

DecompressingByteSource::~DecompressingByteSource()
{
    ReleaseTempFile();
}

DecompressingByteSource::DecompressingByteSource(DecompressingByteSource &&other) noexcept
    : mDisplayPath(std::move(other.mDisplayPath)),
      mEffectivePath(std::move(other.mEffectivePath)),
      mCodec(other.mCodec),
      mCompressedSize(other.mCompressedSize),
      mDecompressedSize(other.mDecompressedSize),
      mOwnsTempFile(other.mOwnsTempFile),
      mDiscardedFirstLine(std::move(other.mDiscardedFirstLine))
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
        mDiscardedFirstLine = std::move(other.mDiscardedFirstLine);
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
        // Best-effort: a leaked temp is a footprint issue, not
        // a correctness one.
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

const std::string &DecompressingByteSource::DiscardedFirstLine() const noexcept
{
    return mDiscardedFirstLine;
}

DecompressingByteSource::Codec DecompressingByteSource::SniffCodec(const std::filesystem::path &input) noexcept
{
    std::error_code ec;
    // Empty / missing / directory / unreadable -> None. `file_size`
    // sets `ec` for all three failure cases.
    // MSVC's <filesystem> flag-cast trips clang-analyzer's enum-cast check.
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    const auto size = std::filesystem::file_size(input, ec);
    if (ec || size == 0)
    {
        return Codec::None;
    }
    const std::vector<std::uint8_t> magic = TryReadMagic(input, MAGIC_MAX_BYTES);
    if (magic.empty())
    {
        return Codec::None;
    }
    return DetectCodec({magic.data(), magic.size()});
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
