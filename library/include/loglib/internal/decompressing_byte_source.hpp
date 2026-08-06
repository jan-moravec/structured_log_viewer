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

/// Thrown when decompression is cancelled.
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

/// Thrown when decompressed output exceeds the configured cap.
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

/// RAII decoder for gzip, bzip2, xz, and zstd files.
///
/// Compressed input is streamed to an owned temp file exposed through
/// `EffectivePath()`. Plain input is returned unchanged. Not thread-safe.
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

    /// Default 32 GiB decompressed-output cap.
    static constexpr std::size_t DEFAULT_MAX_DECOMPRESSED_BYTES = std::size_t{32} << 30;

    struct Options
    {
        /// Hard cap; throws `DecompressionSizeCapExceeded` if
        /// exceeded. Zero disables the cap.
        std::size_t maxDecompressedBytes = DEFAULT_MAX_DECOMPRESSED_BYTES;
        /// Remove the first line and expose it via `DiscardedFirstLine()`.
        bool discardFirstLine = false;
        /// Maximum buffered first-line size.
        std::size_t maxDiscardedFirstLineBytes = 64U * 1024U * 1024U;
    };

    /// Sniff @p input and decode compressed content to a temp file.
    /// Progress and cancellation are checked between input chunks.
    DecompressingByteSource(
        std::filesystem::path input, const ProgressCallback &progress = {}, const StopToken &stopToken = {}
    );

    /// Explicit-@p options overload. Kept separate from the default
    /// above because some clang versions diagnose an aggregate
    /// default parameter for a member of an incomplete class.
    DecompressingByteSource(
        std::filesystem::path input, const ProgressCallback &progress, const StopToken &stopToken, Options options
    );

    /// Detect a codec from up to six magic bytes. Plain, empty, and
    /// unreadable files return `Codec::None`.
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

    /// Bytes stripped by `Options::discardFirstLine`, without the
    /// terminating newline. Empty when the option was off.
    [[nodiscard]] const std::string &DiscardedFirstLine() const noexcept;

private:
    void ReleaseTempFile() noexcept;

    std::filesystem::path mDisplayPath;
    std::filesystem::path mEffectivePath;
    Codec mCodec = Codec::None;
    std::size_t mCompressedSize = 0;
    std::size_t mDecompressedSize = 0;
    /// True when `mEffectivePath` is a temp file owned by this object.
    bool mOwnsTempFile = false;
    std::string mDiscardedFirstLine;
};

/// Human-readable codec name (`"gzip"`, `"bzip2"`, `"xz"`, `"zstd"`,
/// `"none"`). The returned view has static storage duration.
[[nodiscard]] std::string_view CodecName(DecompressingByteSource::Codec codec) noexcept;

} // namespace loglib::internal
