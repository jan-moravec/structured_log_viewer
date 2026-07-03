#pragma once

#include <loglib/stop_token.hpp>

#include <cstddef>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string_view>

namespace loglib::internal
{

/// Thrown by `DecompressingByteSource` when its `StopToken` observes a
/// stop request mid-decode. Distinct from `std::runtime_error` so
/// callers can surface "cancelled" separately from "corrupt".
class DecompressionCancelled : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/// RAII pre-open filter for transparent decompression of the four
/// canonical single-file codecs (`gzip`, `bzip2`, `xz`, `zstd`).
///
/// The constructor sniffs the first bytes of @p input for a codec magic
/// number. When a codec is detected, it streams the decompressed output
/// into a temp file under `std::filesystem::temp_directory_path()`;
/// `EffectivePath()` returns that temp path. When no codec matches (or
/// the file is empty), `EffectivePath() == DisplayPath() == input`.
///
/// The temp file's lifetime is bound to this object: the destructor
/// deletes it. Downstream code must keep this object alive for as long
/// as it holds any reference (e.g. mmap) into the temp file.
///
/// Failure modes:
///   - Cancellation (StopToken observes stop): throws
///     `DecompressionCancelled` after cleaning up the partial temp file.
///   - Any other IO / codec failure: throws `std::runtime_error` with
///     codec name + byte offset context, again after cleaning up.
///
/// Thread-safety: instances are not thread-safe. The constructor and
/// destructor must run on the same thread; the object itself can be
/// safely moved but not concurrently accessed from multiple threads.
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
        /// Compressed bytes consumed since decode started.
        std::size_t bytesIn = 0;
        /// Compressed file size in bytes (known up front from
        /// `std::filesystem::file_size`).
        std::size_t totalBytesIn = 0;
    };

    using ProgressCallback = std::function<void(const Progress &)>;

    /// Sniff @p input and (if compressed) decode to a temp file. The
    /// callback is invoked on the calling thread after each 64 KiB
    /// input chunk; may be null. The token is polled between chunks;
    /// if `stop_requested()` becomes true, throws
    /// `DecompressionCancelled` after cleanup.
    DecompressingByteSource(
        std::filesystem::path input, ProgressCallback progress = {}, StopToken stopToken = {}
    );

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
