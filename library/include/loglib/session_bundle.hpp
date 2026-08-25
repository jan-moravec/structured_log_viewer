#pragma once

#include "loglib/log_configuration.hpp"
#include "loglib/stop_token.hpp"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace loglib
{

class LogTable;

/** @brief File extension for a session bundle produced by `WriteSessionBundle`. */
constexpr const char *SESSION_BUNDLE_EXTENSION = ".slvbundle";

/** @brief Encoder options for the single zstd frame. */
struct SessionBundleWriteOptions
{
    /** @brief zstd compression level; 3 is the balanced default. */
    int compressionLevel = 3;

    /** @brief Number of zstd workers. Zero uses zstd's single-threaded path. */
    int totalWorkers = 0;

    /**
     * @brief Cooperative cancellation. The writer removes its staging file
     * before throwing `SessionBundleCancelled`.
     */
    StopToken stopToken;

    /** @brief Progress in source-model rows. Called on the writer thread. */
    std::function<void(std::uint64_t rowsWritten, std::uint64_t rowsTotal)> progress;

    /**
     * @brief Convert source paths to the same form as `AnchorEntry::locator`
     * before remapping anchors. Defaults to a raw UTF-8 path.
     */
    std::function<std::string(const std::filesystem::path &)> canonicalizeSourceLocator;
};

/** @brief Metadata stored on the first decompressed JSONL line. */
struct SessionBundleMetadata
{
    std::uint32_t formatVersion = 0;
    std::uint64_t rowCount = 0;
    LogConfiguration configuration;
};

/** @brief Thrown when bundle writing is cancelled. */
class SessionBundleCancelled : public std::exception
{
public:
    [[nodiscard]] const char *what() const noexcept override
    {
        return "Session bundle operation cancelled";
    }
};

/** @brief Thrown when the bundle version is unsupported. */
class SessionBundleVersionError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/** @brief Generic bundle metadata failure. */
class SessionBundleReadError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief Write metadata and every retained row as JSONL in one checksummed
 * zstd frame. Replace the destination only after a durable flush.
 */
void WriteSessionBundle(
    const LogTable &table,
    const LogConfiguration &configuration,
    const std::filesystem::path &destination,
    const SessionBundleWriteOptions &options = {}
);

/** @brief Parse and validate the first decompressed JSONL line. */
[[nodiscard]] SessionBundleMetadata ParseSessionBundleMetadata(std::string_view json);

/**
 * @brief Return whether @p file has zstd input accepted by the decoder.
 * Missing, unreadable, and non-zstd files return false.
 */
[[nodiscard]] bool LooksLikeSessionBundle(const std::filesystem::path &file) noexcept;

/** @brief Exact on-disk version emitted and accepted by this build. */
constexpr std::uint32_t SESSION_BUNDLE_FORMAT_VERSION = 1;

/** @brief Maximum row count accepted by the writer and reader. */
constexpr std::uint64_t SESSION_BUNDLE_MAX_ROWS = 1'000'000'000ULL;

} // namespace loglib
