#pragma once

#include <loglib/stop_token.hpp>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace loglib::internal
{

/// Thrown when the `StopToken` observes a stop mid-decode.
/// Deliberately not a `std::runtime_error`: callers must distinguish
/// user cancels (toast, keep draining) from I/O / codec failures
/// (error dialog), and `catch (const std::runtime_error &)` must
/// not absorb cancels.
class DecompressionCancelled : public std::exception
{
public:
    // Not `noexcept`: by-value copy of an lvalue would allocate.
    explicit DecompressionCancelled(std::string what)
        : mWhat(std::move(what))
    {
    }

    [[nodiscard]] const char *what() const noexcept override
    {
        return mWhat.c_str();
    }

private:
    std::string mWhat;
};

/// Thrown when the decompressed payload would exceed the configured
/// size cap. Distinct type so callers can surface a specific "too
/// large" message. Not a `std::runtime_error`, same rationale as
/// `DecompressionCancelled`.
class DecompressionSizeCapExceeded : public std::exception
{
public:
    explicit DecompressionSizeCapExceeded(std::string what)
        : mWhat(std::move(what))
    {
    }

    [[nodiscard]] const char *what() const noexcept override
    {
        return mWhat.c_str();
    }

private:
    std::string mWhat;
};

/// RAII pre-open filter for transparent decompression of `gzip`,
/// `bzip2`, `xz`, and `zstd` single-file streams.
///
/// The ctor sniffs @p input for a codec magic; when found, it streams
/// the output into a temp file under `temp_directory_path()` and
/// exposes it via `EffectivePath()`. Uncompressed / empty input:
/// `EffectivePath() == DisplayPath() == input`.
///
/// The temp file's lifetime is bound to this object. Keep it alive
/// for as long as any reference (e.g. mmap) into the temp file lives.
///
/// Failure modes:
///   - Cancellation: throws `DecompressionCancelled` after cleanup.
///   - I/O / codec failure: throws `std::runtime_error` with codec
///     name + byte-offset context after cleanup.
///
/// Not thread-safe. Movable, but do not access an instance from
/// multiple threads concurrently.
class DecompressingByteSource
{
public:
    enum class Codec
    {
        None,
        Gzip,
        Bzip2,
        Xz,
        Zstd,
    };

    struct Progress
    {
        /// Compressed bytes consumed so far.
        std::size_t bytesIn = 0;
        /// Total compressed size (from `file_size`).
        std::size_t totalBytesIn = 0;
    };

    using ProgressCallback = std::function<void(const Progress &)>;

    /// Default size cap on decompressed output. 32 GiB comfortably
    /// covers realistic logs (even a highly compressible 500 MiB
    /// `.xz`) while bounding zip-bomb inputs.
    static constexpr std::size_t DEFAULT_MAX_DECOMPRESSED_BYTES = std::size_t{32} << 30;

    struct Options
    {
        /// Hard cap; throws `DecompressionSizeCapExceeded` if
        /// exceeded. Zero disables the cap.
        std::size_t maxDecompressedBytes = DEFAULT_MAX_DECOMPRESSED_BYTES;
    };

    /// Sniff @p input and (if compressed) decode to a temp file.
    /// @p progress fires after each 64 KiB input chunk on the calling
    /// thread; may be null. @p stopToken is polled between chunks;
    /// on stop, throws `DecompressionCancelled` after cleanup.
    DecompressingByteSource(
        std::filesystem::path input, const ProgressCallback &progress = {}, const StopToken &stopToken = {}
    );

    /// Explicit-@p options overload. Kept separate from the default
    /// above because some clang versions diagnose an aggregate
    /// default parameter for a member of an incomplete class.
    DecompressingByteSource(
        std::filesystem::path input, const ProgressCallback &progress, const StopToken &stopToken, Options options
    );

    /// noexcept magic-byte sniff (reads up to 6 bytes). Returns the
    /// detected codec, or `Codec::None` for uncompressed / empty /
    /// unreadable files. Callers use this to pick "sync fast path
    /// vs async worker" without paying for a full ctor. I/O failure
    /// collapses to `Codec::None` so the downstream `LogFile` ctor
    /// produces the canonical open-error message.
    [[nodiscard]] static Codec SniffCodec(const std::filesystem::path &input) noexcept;

    ~DecompressingByteSource();

    DecompressingByteSource(const DecompressingByteSource &) = delete;
    DecompressingByteSource &operator=(const DecompressingByteSource &) = delete;

    DecompressingByteSource(DecompressingByteSource &&other) noexcept;
    DecompressingByteSource &operator=(DecompressingByteSource &&other) noexcept;

    /// User-facing path (e.g. `app.log.gz`) — always the input path.
    [[nodiscard]] const std::filesystem::path &DisplayPath() const noexcept;

    /// Path downstream code should mmap / probe. Equal to
    /// `DisplayPath()` when the input was not compressed.
    [[nodiscard]] const std::filesystem::path &EffectivePath() const noexcept;

    [[nodiscard]] bool WasDecompressed() const noexcept;
    [[nodiscard]] Codec DetectedCodec() const noexcept;

    /// Size of the compressed input, in bytes.
    [[nodiscard]] std::size_t CompressedSize() const noexcept;

    /// Size of the decompressed temp file, in bytes. Zero when
    /// `WasDecompressed()` is false.
    [[nodiscard]] std::size_t DecompressedSize() const noexcept;

private:
    void ReleaseTempFile() noexcept;

    std::filesystem::path mDisplayPath;
    std::filesystem::path mEffectivePath;
    Codec mCodec = Codec::None;
    std::size_t mCompressedSize = 0;
    std::size_t mDecompressedSize = 0;
    /// True when `mEffectivePath` is a temp file owned by this object.
    bool mOwnsTempFile = false;
};

/// Human-readable codec name (`"gzip"`, `"bzip2"`, `"xz"`, `"zstd"`,
/// `"none"`). The returned view has static storage duration.
[[nodiscard]] std::string_view CodecName(DecompressingByteSource::Codec codec) noexcept;

} // namespace loglib::internal
