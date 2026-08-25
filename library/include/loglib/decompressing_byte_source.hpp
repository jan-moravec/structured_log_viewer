#pragma once

#include <loglib/stop_token.hpp>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace loglib
{

/** @brief Thrown when decompression is cancelled. */
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

/** @brief Thrown when decompressed output exceeds the configured cap. */
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

/**
 * @brief RAII decoder for gzip, bzip2, xz, and zstd files.
 *
 * Compressed input is streamed to an owned temp file exposed through
 * `EffectivePath()`. Plain input is returned unchanged. Not thread-safe.
 */
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
        /** @brief Compressed bytes consumed so far. */
        std::size_t bytesIn = 0;
        /** @brief Total compressed size from `file_size`. */
        std::size_t totalBytesIn = 0;
    };

    using ProgressCallback = std::function<void(const Progress &)>;

    /** @brief Default 32 GiB decompressed-output cap. */
    static constexpr std::size_t DEFAULT_MAX_DECOMPRESSED_BYTES = std::size_t{32} << 30;

    /** @brief Default 64 MiB cap on the discarded first line. */
    static constexpr std::size_t DEFAULT_MAX_DISCARDED_FIRST_LINE_BYTES = std::size_t{64} << 20;

    struct Options
    {
        /**
         * @brief Hard cap on decompressed output.
         *
         * Throws `DecompressionSizeCapExceeded` if exceeded. Zero
         * disables the cap.
         */
        std::size_t maxDecompressedBytes = DEFAULT_MAX_DECOMPRESSED_BYTES;
        /** @brief When true, strips the first line and exposes it via `DiscardedFirstLine()`. */
        bool discardFirstLine = false;
        /** @brief Maximum buffered first-line size. */
        std::size_t maxDiscardedFirstLineBytes = DEFAULT_MAX_DISCARDED_FIRST_LINE_BYTES;
    };

    /**
     * @brief Sniffs @p input and decodes compressed content to a temp file.
     *
     * Progress and cancellation are checked between input chunks.
     *
     * @param input Path to sniff and decode.
     * @param progress Optional progress callback.
     * @param stopToken Cancellation token observed between chunks.
     */
    DecompressingByteSource(
        std::filesystem::path input, const ProgressCallback &progress = {}, const StopToken &stopToken = {}
    );

    /**
     * @brief Constructs a decoder with explicit @p options.
     *
     * Kept separate from the default constructor because some clang
     * versions diagnose an aggregate default parameter for a member of
     * an incomplete class.
     *
     * @param input Path to sniff and decode.
     * @param progress Optional progress callback.
     * @param stopToken Cancellation token observed between chunks.
     * @param options Decoder options including size caps.
     */
    DecompressingByteSource(
        std::filesystem::path input, const ProgressCallback &progress, const StopToken &stopToken, Options options
    );

    /**
     * @brief Detects a codec from up to six magic bytes.
     *
     * Plain, empty, and unreadable files return `Codec::None`.
     *
     * @param input Path to sniff.
     * @return Detected codec, or `Codec::None`.
     */
    [[nodiscard]] static Codec SniffCodec(const std::filesystem::path &input) noexcept;

    ~DecompressingByteSource();

    DecompressingByteSource(const DecompressingByteSource &) = delete;
    DecompressingByteSource &operator=(const DecompressingByteSource &) = delete;

    DecompressingByteSource(DecompressingByteSource &&other) noexcept;
    DecompressingByteSource &operator=(DecompressingByteSource &&other) noexcept;

    /** @brief Returns the user-facing path; always the input path. */
    [[nodiscard]] const std::filesystem::path &DisplayPath() const noexcept;

    /**
     * @brief Returns the path downstream code should mmap or probe.
     *
     * Equal to `DisplayPath()` when the input was not compressed.
     */
    [[nodiscard]] const std::filesystem::path &EffectivePath() const noexcept;

    [[nodiscard]] bool WasDecompressed() const noexcept;
    [[nodiscard]] Codec DetectedCodec() const noexcept;

    /** @brief Returns the compressed input size in bytes. */
    [[nodiscard]] std::size_t CompressedSize() const noexcept;

    /**
     * @brief Returns the decompressed temp-file size in bytes.
     * @return Zero when `WasDecompressed()` is false.
     */
    [[nodiscard]] std::size_t DecompressedSize() const noexcept;

    /**
     * @brief Returns bytes stripped by `Options::discardFirstLine`.
     *
     * The view excludes the terminating newline. Empty when the option
     * was off.
     */
    [[nodiscard]] const std::string &DiscardedFirstLine() const noexcept;

private:
    void ReleaseTempFile() noexcept;

    std::filesystem::path mDisplayPath;
    std::filesystem::path mEffectivePath;
    Codec mCodec = Codec::None;
    std::size_t mCompressedSize = 0;
    std::size_t mDecompressedSize = 0;
    /** @brief True when `mEffectivePath` is a temp file owned by this object. */
    bool mOwnsTempFile = false;
    std::string mDiscardedFirstLine;
};

/**
 * @brief Returns a human-readable codec name.
 *
 * Names are `"gzip"`, `"bzip2"`, `"xz"`, `"zstd"`, and `"none"`. The
 * returned view has static storage duration.
 *
 * @param codec Codec to name.
 * @return Static name view.
 */
[[nodiscard]] std::string_view CodecName(DecompressingByteSource::Codec codec) noexcept;

} // namespace loglib
