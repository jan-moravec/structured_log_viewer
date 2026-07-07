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

// POSIX-only headers for the exclusive-create temp file path. Kept
// out of the Windows build so MSVC (which lacks `<fcntl.h>`'s POSIX
// constants + `<sys/stat.h>`) doesn't need to fake them.
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

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

/// A valid `.zst` stream may begin with a **skippable frame** instead
/// of the normal zstd frame magic above. Per RFC 8878 §3.1.2 the
/// skippable-frame magic is the little-endian 32-bit range
/// `[0x184D2A50, 0x184D2A5F]`. In little-endian byte order that lays
/// out on disk as `<byte0 in 0x50..0x5F> 0x2A 0x4D 0x18`, so only the
/// low nibble of the first byte varies. `ZSTD_decompressStream`
/// transparently skips these frames on its way to the next real
/// frame, so detecting them as `Codec::Zstd` lets us handle producers
/// that prepend one (e.g. dictionary IDs, custom container headers)
/// without silently misclassifying the file as uncompressed and
/// dumping raw compressed bytes into the JSONL parser.
[[nodiscard]] bool IsZstdSkippableMagic(std::span<const std::uint8_t> haystack) noexcept
{
    return haystack.size() >= 4 && (haystack[0] & 0xF0) == 0x50 && haystack[1] == 0x2A && haystack[2] == 0x4D &&
           haystack[3] == 0x18;
}

[[nodiscard]] bool StartsWith(std::span<const std::uint8_t> haystack, std::span<const std::uint8_t> needle) noexcept
{
    if (haystack.size() < needle.size())
    {
        return false;
    }
    return std::memcmp(haystack.data(), needle.data(), needle.size()) == 0;
}

/// Read up to @p max bytes from the beginning of @p path. On IO error
/// or missing file, throws `std::runtime_error`. Used by the ctor
/// which has a path already known to exist (from `file_size`) and
/// wants a hard error when the read subsequently fails; the noexcept
/// `SniffCodec` public helper uses `TryReadMagic` instead.
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

/// Non-throwing sibling of `ReadMagic`. Returns an empty vector on
/// any I/O error so `SniffCodec` can safely present `Codec::None`
/// upstream and let the downstream `LogFile` ctor produce the
/// canonical open-error message.
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
    if (StartsWith(magic, {kZstdMagic.data(), kZstdMagic.size()}) || IsZstdSkippableMagic(magic))
    {
        return Codec::Zstd;
    }
    return Codec::None;
}

/// Build a unique temp file path under `std::filesystem::temp_directory_path()`.
/// Uses a random suffix seeded from `std::random_device` + steady-clock
/// + a process-local counter so parallel workers don't collide. The
/// file is not created here — the caller opens it exclusively via
/// `OpenExclusive`.
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

// Open @p path for writing, failing (rather than truncating) if a
// file already exists at that path.
//
// Windows: MSVC's fopen family accepts the C11 `x` mode letter --
// combined with `w` it means "create new file; fail if it already
// exists". `N` keeps the underlying HANDLE non-inheritable so a
// child process spawned during a long decompression can't
// accidentally observe our temp fd. The path is passed through
// `_wfopen_s` so long paths and non-ASCII characters survive.
//
// Confidentiality note (Windows): the temp file inherits the DACL
// of its parent directory. In practice this is the per-user
// `%TEMP%` (`C:\Users\<name>\AppData\Local\Temp`), which is
// already restricted to the owning user + SYSTEM + Administrators.
// If `%TEMP%` has been redirected to a shared location (some
// service accounts, unusual Citrix / Terminal Services setups) the
// decompressed contents will be readable by anyone with local
// access to that directory. We deliberately do not set an explicit
// DACL via `CreateFileW` + `SECURITY_ATTRIBUTES`: the extra Win32
// surface is not worth carrying for a scenario the default temp
// layout already handles correctly.
//
// POSIX: `open(O_WRONLY | O_CREAT | O_EXCL, 0600)` is the canonical
// safe-tempfile idiom; the exclusive-create is enforced by the
// kernel (also on network filesystems that support the flag),
// which is stronger than the userspace random-name check that
// `MakeTempPath` performs. The `0600` mode restricts read/write
// to the owner so decompressed log contents are not visible to
// other local users on a shared `/tmp`. `O_CLOEXEC` matches the
// Windows `N` no-inherit intent. `fdopen(fd, "wb")` layers a
// FILE* over the fd so the rest of the decompression pipeline is
// unchanged.
/// Sentinel `errno` / `errno_t` we treat as "temp path collided; try
/// another". Anything else is a hard error. `EEXIST` on POSIX is the
/// canonical "file already exists" from `O_EXCL`. On Windows, MSVC's
/// `_wfopen_s(..., L"wbxN")` maps a pre-existing target file to
/// `EEXIST` too, matching the POSIX contract.
constexpr int kTempFileCollisionErrno = EEXIST;

/// Result of a single `OpenExclusive` attempt. `collided == true` when
/// the failure was specifically "target path already existed"; the
/// caller then retries with a fresh `MakeTempPath()`.
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
    // "wbxN" -> write, binary, exclusive create (C11 `x`), no-inherit.
    // `_wfopen_s` handles wide paths.
    const errno_t err = ::_wfopen_s(&raw, path.native().c_str(), L"wbxN");
    if (err != 0 || raw == nullptr)
    {
        return OpenExclusiveResult{FileHandle(), err == kTempFileCollisionErrno, err};
    }
    return OpenExclusiveResult{FileHandle(raw), false, 0};
#else
    // `O_CLOEXEC` is POSIX.1-2008; every platform we support (glibc,
    // musl, Darwin, BSD) has it. Guard just in case some minimal
    // platform lacks the macro.
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
    // Use `path.c_str()` (native `char*` on POSIX) instead of
    // `path.string().c_str()`. `path::string()` transcodes into the
    // narrow "generic" encoding, which on some systems (notably
    // POSIX locales where the filesystem encoding is not UTF-8, or
    // Darwin's NFD normalisation quirks) can round-trip a valid
    // native path into an invalid or differently-normalised one and
    // fail the `open()` even though the input file itself was
    // opened fine via `std::ifstream(path)` (which uses the
    // implementation's own path handling). `c_str()` is the raw
    // native representation and matches `_wfopen_s(path.native())`
    // on the Windows branch for the same reason.
    const int fd = ::open(path.c_str(), flags, S_IRUSR | S_IWUSR);
    if (fd < 0)
    {
        const int savedErrno = errno;
        return OpenExclusiveResult{FileHandle(), savedErrno == kTempFileCollisionErrno, savedErrno};
    }
    std::FILE *raw = ::fdopen(fd, "wb");
    if (raw == nullptr)
    {
        const int savedErrno = errno;
        // `fdopen` failure means we own the fd + the just-created
        // file; both need cleanup or we leak them. Not a collision
        // (the open succeeded), so this reports as a hard failure.
        ::close(fd);
        std::error_code ignoreEc;
        std::filesystem::remove(path, ignoreEc);
        return OpenExclusiveResult{FileHandle(), false, savedErrno};
    }
    return OpenExclusiveResult{FileHandle(raw), false, 0};
#endif
}

/// Combined `MakeTempPath` + `TryOpenExclusive` retry loop. Extremely
/// unlikely to matter in practice (the temp-path suffix combines 128
/// bits of `random_device` + a steady-clock nanosecond count + a
/// process-local counter), but a burst of parallel worker opens under
/// a shared `%TEMP%` on CI can hit a collision often enough to be
/// worth the retry rather than surfacing a spurious ctor failure. We
/// cap retries so a genuinely broken temp directory (e.g. read-only,
/// or every candidate name blocked by AV) still fails fast rather
/// than looping forever. Returns the created path via @p outPath.
[[nodiscard]] FileHandle OpenExclusiveWithRetry(std::filesystem::path &outPath)
{
    constexpr int kMaxAttempts = 8;
    int lastErrno = 0;
    std::filesystem::path lastPath;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt)
    {
        std::filesystem::path candidate = MakeTempPath();
        OpenExclusiveResult result = TryOpenExclusive(candidate);
        if (result.handle.handle != nullptr)
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
    throw std::runtime_error(
        fmt::format("Failed to create temp file '{}': errno {}", lastPath.string(), lastErrno)
    );
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

/// Write @p bytes bytes from @p data to @p out, then account them
/// against the running decompressed-output counter and enforce the
/// caller-supplied cap. The cap check runs **after** the write so the
/// counter reflects what actually landed on disk; the caller
/// (`DecompressingByteSource` ctor) unlinks the partial temp file in
/// its `catch (...)` when we throw. A cap of 0 disables enforcement
/// (still updates the counter). Every codec's write path routes
/// through here so there is exactly one place to bolt the check on
/// -- do not add codec-local `decompressedSize +=` shortcuts.
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
        throw DecompressionSizeCapExceeded(fmt::format(
            "Decompressed output for '{}' exceeded the {}-byte cap "
            "(would need at least {} bytes)",
            sourcePath.string(),
            maxDecompressedBytes,
            decompressedSize
        ));
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
    std::size_t &decompressedSize,
    std::size_t maxDecompressedBytes
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
        //
        // The extra `ObservePoll` inside the loop bounds cancel
        // latency by ONE `inflate` call (~one 64 KiB output write),
        // not by the whole chunk drain -- highly compressible input
        // (e.g. long runs of the same byte) can expand a 64 KiB
        // compressed chunk into many MB of output, so polling only
        // per-read leaves cancels stuck for seconds. Matches DecodeXz
        // and DecodeZstd's inner-loop polling.
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
            ObservePoll(consumed, totalBytesIn, progress, stopToken);
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
            WriteOutput(out, outBuf.data(), produced, tempPath, sourcePath, decompressedSize, maxDecompressedBytes);
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
    std::size_t &decompressedSize,
    std::size_t maxDecompressedBytes
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
        //
        // Poll cadence: see DecodeGzip's matching comment.
        // `BZ2_bzDecompress` produces at most `outBuf.size()` bytes
        // per call, so the extra poll here caps cancel latency at
        // one bzip2 decode iteration regardless of how much output
        // a single input chunk expands into.
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
            ObservePoll(consumed, totalBytesIn, progress, stopToken);
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
            WriteOutput(out, outBuf.data(), produced, tempPath, sourcePath, decompressedSize, maxDecompressedBytes);
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

// Decodes .xz / .lzma streams via liblzma's *multi-threaded* decoder
// (`lzma_stream_decoder_mt`). liblzma silently falls back to single-
// threaded decoding for streams that either only contain one block
// (the default `xz file` output is single-block) or lack block-header
// size info -- so this MT path is a strict >= for every .xz input, and
// only inputs produced with `xz -T <N>` (or `xz --block-size`) actually
// gain parallel throughput. See the tukaani docs on
// `lzma_stream_decoder_mt` and the `lzma_mt` struct:
//
//   https://tukaani.org/xz/xz-file-format.txt (`Multi-Block` section)
//   https://github.com/tukaani-project/xz/blob/master/src/liblzma/api/lzma/container.h
//
// The drain loop below is unchanged from the single-threaded decoder --
// `lzma_code` returns the same LZMA_OK / LZMA_STREAM_END / error codes,
// and with `mt.timeout = 100` a busy-waiting MT worker yields an idle
// LZMA_OK return every ~100 ms which flows naturally through the
// existing `ObservePoll` between input reads (so Cancel latency stays
// comparable to the single-thread codec paths).
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

    // Worker count. `lzma_cputhreads()` returns 0 when the platform
    // doesn't expose CPU-count info; clamp to 1 so the MT decoder still
    // constructs a valid single-worker pipeline (equivalent to the old
    // single-threaded path).
    std::uint32_t threads = ::lzma_cputhreads();
    if (threads == 0)
    {
        threads = 1;
    }

    lzma_mt mt = {};
    // `LZMA_CONCATENATED` lets us stream through concatenated members
    // (like `xzcat file1.xz file2.xz`) exactly as the ST decoder did.
    mt.flags = LZMA_CONCATENATED;
    mt.threads = threads;
    // 100 ms poll window (the xz CLI uses 300 ms). This bounds the
    // wall-clock gap between the StopToken being set and the next
    // `ObservePoll` seeing it, so Cancel latency stays in the same
    // ballpark as the single-thread codec branches.
    mt.timeout = 100;
    // Hard memory cap: never fail with `LZMA_MEMLIMIT_ERROR`. Any file
    // that opens today under the ST decoder must still open here.
    mt.memlimit_stop = UINT64_MAX;
    // Soft memory cap: when the MT decoder would need more than this
    // to run all workers in parallel, liblzma silently reduces the
    // active thread count (never bailing out). `lzma_physmem() / 4`
    // is the upstream-recommended starting point; when physmem is 0
    // (unknown / restricted container / sandbox) we fall back to a
    // 512 MiB floor so the MT scheduler always has room to breathe.
    const std::uint64_t physmem = ::lzma_physmem();
    mt.memlimit_threading = physmem == 0 ? (std::uint64_t{512} << 20) : (physmem / 4);

    const lzma_ret initRet = ::lzma_stream_decoder_mt(&strm, &mt);
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
        }

        // Poll the stop token + fire progress on EVERY iteration,
        // not just fresh reads. `lzma_code` can spend a long time
        // draining a single 64 KiB input chunk into potentially
        // many megabytes of decompressed output (highly-compressible
        // input like repeating text, or a well-compressed member
        // whose output straddles many output buffers). Without a
        // poll here, cancel latency scales with the decompressed
        // output size instead of the compressed input chunk size.
        // Matches the ObservePoll cadence used in DecodeZstd's
        // outer loop for the same reason.
        ObservePoll(consumed, totalBytesIn, progress, stopToken);

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
    std::size_t maxDecompressedBytes
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

        consumed += static_cast<std::size_t>(gotSigned);

        ZSTD_inBuffer input{inBuf.data(), static_cast<std::size_t>(gotSigned), 0};
        while (input.pos < input.size)
        {
            // Poll on every ZSTD_decompressStream call, not just per
            // read. A highly-compressible zstd frame can spend
            // multiple iterations expanding a 64 KiB compressed chunk
            // into much more decompressed output; polling per-read
            // would leave cancels stuck for the whole drain. Bounds
            // cancel latency at one decompressStream call. Matches
            // DecodeGzip / DecodeBzip2 / DecodeXz's inner-loop
            // polling cadence.
            ObservePoll(consumed, totalBytesIn, progress, stopToken);
            ZSTD_outBuffer output{outBuf.data(), outBuf.size(), 0};
            const std::size_t result = ::ZSTD_decompressStream(dctx, &output, &input);
            if (::ZSTD_isError(result) != 0U)
            {
                throw std::runtime_error(fmt::format(
                    "zstd decode error on '{}' at input byte {} ({})",
                    sourcePath.string(),
                    (consumed - static_cast<std::size_t>(gotSigned)) + input.pos,
                    ::ZSTD_getErrorName(result)
                ));
            }
            WriteOutput(out, output.dst, output.pos, tempPath, sourcePath, decompressedSize, maxDecompressedBytes);
            lastResult = result;
        }

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
    : DecompressingByteSource(std::move(input), std::move(progress), std::move(stopToken), Options{})
{
}

DecompressingByteSource::DecompressingByteSource(
    std::filesystem::path input, ProgressCallback progress, StopToken stopToken, Options options
)
    : mDisplayPath(std::move(input))
    , mEffectivePath(mDisplayPath)
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
    // `OpenExclusiveWithRetry` succeeds -- before that point no file
    // exists on disk, so an early throw (from opening the *input*
    // stream) must NOT trigger `ReleaseTempFile()`. Setting the flag
    // inside the try scope keeps the "owns the file" invariant tight.
    std::ifstream inStream(mDisplayPath, std::ios::binary);
    if (!inStream.is_open())
    {
        throw std::runtime_error(fmt::format("Failed to open '{}' for decompression", mDisplayPath.string()));
    }

    try
    {
        std::filesystem::path tempPath;
        FileHandle outHandle = OpenExclusiveWithRetry(tempPath);
        // Only now does a file exist on disk that we're responsible
        // for cleaning up. Any exception below unwinds through the
        // catch block, which calls ReleaseTempFile().
        mEffectivePath = tempPath;
        mOwnsTempFile = true;

        switch (mCodec)
        {
        case Codec::Gzip:
            DecodeGzip(
                inStream, outHandle, mDisplayPath, tempPath, mCompressedSize, progress, stopToken, mDecompressedSize,
                maxDecompressedBytes
            );
            break;
        case Codec::Bzip2:
            DecodeBzip2(
                inStream, outHandle, mDisplayPath, tempPath, mCompressedSize, progress, stopToken, mDecompressedSize,
                maxDecompressedBytes
            );
            break;
        case Codec::Xz:
            DecodeXz(
                inStream, outHandle, mDisplayPath, tempPath, mCompressedSize, progress, stopToken, mDecompressedSize,
                maxDecompressedBytes
            );
            break;
        case Codec::Zstd:
            DecodeZstd(
                inStream, outHandle, mDisplayPath, tempPath, mCompressedSize, progress, stopToken, mDecompressedSize,
                maxDecompressedBytes
            );
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
        // Restore the effective path to the input so a caller that
        // catches (and inspects `EffectivePath()` for diagnostics)
        // doesn't see a dangling temp-file path we just unlinked.
        mEffectivePath = mDisplayPath;
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

DecompressingByteSource::Codec DecompressingByteSource::SniffCodec(const std::filesystem::path &input) noexcept
{
    std::error_code ec;
    // Empty / missing / unreadable files -> None. `file_size` on a
    // directory sets `ec`; `file_size(missing)` also sets `ec`.
    // Zero-byte files can't hold any codec's frame header.
    const auto size = std::filesystem::file_size(input, ec);
    if (ec || size == 0)
    {
        return Codec::None;
    }
    const std::vector<std::uint8_t> magic = TryReadMagic(input, kMagicMaxBytes);
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
