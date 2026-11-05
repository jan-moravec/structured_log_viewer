/**
 * @brief Reads compressed log files as plain files.
 *
 * A DecompressingByteSource detects gzip, bzip2, xz and zstd files from their first bytes and decompresses them into an
 * owned temporary file. EffectivePath returns that file, so the rest of the library can map a compressed log like a
 * normal one. Plain files are returned unchanged. It supports cancellation and a cap on the decompressed size, and
 * reports both as exceptions.
 *
 * This is part of the ingest module.
 */
#pragma once

#include <loglib/core/stop_token.hpp>

#include <cstddef>
#include <exception>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace loglib
{

/**
 * @brief Decompresses a gzip, bzip2, xz or zstd file into a temporary file and removes that file when destroyed.
 *
 * The constructor does all the work. The decompressed copy is available through EffectivePath. A file that is not
 * compressed is used as it is. The object is not thread-safe.
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

    /** @brief How far a decompression has come. */
    struct Progress
    {
        /** @brief Compressed bytes read so far. */
        std::size_t bytesIn = 0;
        /** @brief Size of the compressed file. */
        std::size_t totalBytesIn = 0;
    };

    /** @brief Called between input chunks with the current progress. */
    using ProgressCallback = std::function<void(const Progress &)>;

    /** @brief The default cap on decompressed output, 32 GiB. */
    static constexpr std::size_t DEFAULT_MAX_DECOMPRESSED_BYTES = std::size_t{32} << 30U;

    /** @brief The default cap on the size of the discarded first line, 64 MiB. */
    static constexpr std::size_t DEFAULT_MAX_DISCARDED_FIRST_LINE_BYTES = std::size_t{64} << 20U;

    struct Options
    {
        /**
         * @brief The most decompressed bytes allowed. Going past it throws LimitError. Zero turns
         * the cap off.
         */
        std::size_t maxDecompressedBytes = DEFAULT_MAX_DECOMPRESSED_BYTES;

        /**
         * @brief Removes the first line from the output and keeps it for DiscardedFirstLine.
         *
         * Only zstd input supports it. Any other input, including a file that is not compressed or is empty, makes the
         * constructor throw.
         */
        bool discardFirstLine = false;

        /** @brief The longest first line that is kept when discardFirstLine is set. */
        std::size_t maxDiscardedFirstLineBytes = DEFAULT_MAX_DISCARDED_FIRST_LINE_BYTES;
    };

    /**
     * @brief Detects the codec of a file and decompresses it into a temporary file.
     *
     * Progress and the stop token are checked between input chunks. A stop throws OperationCancelled.
     */
    explicit DecompressingByteSource(
        std::filesystem::path input, const ProgressCallback &progress = {}, const StopToken &stopToken = {}
    );

    /**
     * @brief Like the first constructor, with explicit options.
     *
     * It is a separate overload because some clang versions reject a default argument of an aggregate type for a member
     * of a class that is still incomplete.
     */
    DecompressingByteSource(
        std::filesystem::path input, const ProgressCallback &progress, const StopToken &stopToken, Options options
    );

    /** @brief Finds the codec from the first bytes of a file. Plain, empty and unreadable files give Codec::None. */
    [[nodiscard]] static Codec SniffCodec(const std::filesystem::path &input) noexcept;

    ~DecompressingByteSource();

    DecompressingByteSource(const DecompressingByteSource &) = delete;
    DecompressingByteSource &operator=(const DecompressingByteSource &) = delete;

    DecompressingByteSource(DecompressingByteSource &&other) noexcept;
    DecompressingByteSource &operator=(DecompressingByteSource &&other) noexcept;

    /** @brief The path to show to the user. It is always the path that was passed in. */
    [[nodiscard]] const std::filesystem::path &DisplayPath() const noexcept;

    /**
     * @brief The path to read, map or probe. It is the decompressed copy, or the input path if the file was not
     * compressed.
     */
    [[nodiscard]] const std::filesystem::path &EffectivePath() const noexcept;

    [[nodiscard]] bool WasDecompressed() const noexcept;
    [[nodiscard]] Codec DetectedCodec() const noexcept;

    /** @brief The size of the input file in bytes. */
    [[nodiscard]] std::size_t CompressedSize() const noexcept;

    /** @brief The size of the decompressed copy in bytes, or zero if the file was not decompressed. */
    [[nodiscard]] std::size_t DecompressedSize() const noexcept;

    /** @brief The first line that discardFirstLine removed, without its newline. Empty if the option was off. */
    [[nodiscard]] const std::string &DiscardedFirstLine() const noexcept;

private:
    void ReleaseTempFile() noexcept;

    std::filesystem::path mDisplayPath;
    std::filesystem::path mEffectivePath;
    Codec mCodec = Codec::None;
    std::size_t mCompressedSize = 0;
    std::size_t mDecompressedSize = 0;
    /** @brief True when the effective path is a temporary file owned by this object, which deletes it. */
    bool mOwnsTempFile = false;
    std::string mDiscardedFirstLine;
};

/** @brief The name of a codec: gzip, bzip2, xz, zstd or none. The view is valid for the whole program. */
[[nodiscard]] std::string_view CodecName(DecompressingByteSource::Codec codec) noexcept;

} // namespace loglib
