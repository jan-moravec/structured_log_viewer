// Fuzz target for the decompressors. The input is written to a temporary file and opened as a compressed log. The first
// bytes pick the codec, so one target covers gzip, bzip2, xz and zstd. A failure reported as a loglib::Error is a
// result. Any other exception is a finding.

#include "fuzz_common.hpp"

#include "loglib/core/error.hpp"
#include "loglib/ingest/decompressing_byte_source.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

namespace
{

namespace fs = std::filesystem;

/** @brief The most output a run may produce. A bigger stream is a LimitError, which keeps a bomb cheap. */
constexpr std::size_t MAX_OUTPUT_BYTES = std::size_t{8} << 20U;

/** @brief A temporary file that is removed when it goes out of scope. */
class TempFile
{
public:
    explicit TempFile(fs::path path)
        : mPath(std::move(path))
    {
    }

    ~TempFile()
    {
        std::error_code ignored;
        fs::remove(mPath, ignored);
    }

    TempFile(const TempFile &) = delete;
    TempFile &operator=(const TempFile &) = delete;

    [[nodiscard]] const fs::path &Path() const noexcept
    {
        return mPath;
    }

private:
    fs::path mPath;
};

fs::path UniqueInputPath()
{
    // One name per process, so several fuzzing processes can share the temporary directory.
    static const std::string NAME = "slv-fuzz-decompress-" + std::to_string(std::random_device{}()) + ".bin";
    return fs::temp_directory_path() / NAME;
}

void Decompress(const fs::path &path, bool discardFirstLine)
{
    try
    {
        loglib::DecompressingByteSource::Options options;
        options.maxDecompressedBytes = MAX_OUTPUT_BYTES;
        options.discardFirstLine = discardFirstLine;
        const loglib::DecompressingByteSource source(path, {}, {}, options);
        (void)source.DecompressedSize();
        (void)source.DiscardedFirstLine();
    }
    catch (const loglib::Error &) // NOLINT(bugprone-empty-catch)
    {
        // IoError, FormatError or LimitError: the documented results for bad input.
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size)
{
    const TempFile input(UniqueInputPath());
    {
        std::ofstream stream(input.Path(), std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    }

    (void)loglib::DecompressingByteSource::SniffCodec(input.Path());
    Decompress(input.Path(), false);
    Decompress(input.Path(), true);
    return 0;
}
