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

/// Thrown by `DecompressingByteSource` when its `StopToken` observes a
/// stop request mid-decode. **Deliberately not** a subclass of
/// `std::runtime_error`: the constructor's other failure surfaces
/// (short read, codec error, temp write) throw `std::runtime_error`,
/// and callers need to distinguish "user cancelled" (surface as toast,
/// continue draining the queue) from "input corrupt / IO failed"
/// (surface as an `Error Decompressing File` batch). Making cancel a
/// sibling exception type instead of a derived one turns
/// `catch (const std::runtime_error &)` into a compile-time forcing
/// function -- callers can no longer accidentally lump cancel in with
/// codec failures.
class DecompressionCancelled : public std::exception
{
public:
    // Not `noexcept`: the pass-by-value parameter copy may allocate
    // if the caller passes an lvalue. Callers in practice pass
    // rvalue string literals or moved fmt::format() results, so
    // this remains cheap.
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

/// Thrown by the `DecompressingByteSource` constructor when the
/// decompressed payload would exceed the configured size cap. Distinct
/// exception type so callers can surface a specific "too large" toast
/// / dialog rather than a generic parse error; also so a future retry
/// UI (e.g. "raise the cap and try again") has a clean type to catch
/// on. Not derived from `std::runtime_error` for the same reason as
/// `DecompressionCancelled`.
class DecompressionSizeCapExceeded : public std::exception
{
public:
    // See DecompressionCancelled: pass-by-value + a `noexcept`
    // guarantee would misrepresent the parameter copy, which may
    // allocate.
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

    /// Default size cap on decompressed output. 32 GiB is chosen so
    /// that no realistic log file trips it -- even a very
    /// compressible 500 MiB `.xz` stays well under this ceiling --
    /// while still bounding zip-bomb / disk-exhaustion attacks
    /// where a small compressed input would expand into TB of
    /// output. Callers with tighter needs (headless tools, sandboxed
    /// CI, memory-constrained embedded consumers) can lower it via
    /// the `Options`-taking constructor overload.
    static constexpr std::size_t kDefaultMaxDecompressedBytes = std::size_t{32} << 30;

    struct Options
    {
        /// Hard cap on decompressed output bytes. Once the accumulated
        /// output exceeds this value the decoder throws
        /// `DecompressionSizeCapExceeded` after cleaning up the
        /// partial temp file. A value of 0 disables the cap.
        std::size_t maxDecompressedBytes = kDefaultMaxDecompressedBytes;
    };

    /// Sniff @p input and (if compressed) decode to a temp file. The
    /// callback is invoked on the calling thread after each 64 KiB
    /// input chunk; may be null. The token is polled between chunks;
    /// if `stop_requested()` becomes true, throws
    /// `DecompressionCancelled` after cleanup. Uses the default
    /// `Options` (32 GiB decompressed-size cap).
    ///
    /// `progress` and `stopToken` are taken by const reference:
    /// their callables/state are only used during construction, so
    /// there is no reason to pay for a copy per invocation.
    DecompressingByteSource(
        std::filesystem::path input, const ProgressCallback &progress = {}, const StopToken &stopToken = {}
    );

    /// Same as above but with explicit @p options. Split into a
    /// separate overload rather than expressed as `Options = {}`
    /// on the primary ctor because using an aggregate-default
    /// (`Options{}`) as a default argument for a member function
    /// of the enclosing class triggers a "default member
    /// initializer needed within definition of enclosing class
    /// outside of member functions" diagnostic on some clang
    /// versions -- Options's own default member initializer for
    /// `maxDecompressedBytes` isn't guaranteed to be usable until
    /// the enclosing class is complete. Two overloads sidesteps
    /// that entirely with no runtime cost.
    DecompressingByteSource(
        std::filesystem::path input, const ProgressCallback &progress, const StopToken &stopToken, Options options
    );

    /// Cheap up-front sniff: opens @p input, reads at most 6 bytes,
    /// and returns the codec its magic prefix identifies (or
    /// `Codec::None` for uncompressed / empty / unreadable files).
    /// Callers use this to decide "sync fast path vs async worker"
    /// without paying for a full construction. Any I/O failure
    /// collapses to `Codec::None` on purpose so the downstream
    /// `LogFile` ctor produces the canonical open-error message
    /// rather than a duplicate from here. Kept in the library so
    /// there is exactly one magic-byte table across the codebase.
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
