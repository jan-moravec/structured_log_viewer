/**
 * @brief Reading and writing session bundle files.
 *
 * A session bundle is one zstd-compressed file with the extension slvbundle. Its first line holds metadata: the format
 * version, the row count and the configuration. Every following line is one row as normalized JSON. WriteSessionBundle
 * creates a bundle from a LogTable and a configuration. LooksLikeSessionBundle checks a file,
 * ParseSessionBundleMetadata reads the metadata line, and the reading code in the sources loads the rows back.
 *
 * This is part of the output module.
 */
#pragma once

#include "loglib/core/stop_token.hpp"
#include "loglib/schema/log_configuration.hpp"

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

/** @brief The file extension of a session bundle. */
constexpr const char *SESSION_BUNDLE_EXTENSION = ".slvbundle";

/** @brief Settings for writing a bundle, which is one zstd frame. */
struct SessionBundleWriteOptions
{
    /** @brief The zstd compression level. 3 is a balance between speed and size. */
    int compressionLevel = 3;

    /** @brief The number of zstd worker threads. Zero compresses on the calling thread. */
    int totalWorkers = 0;

    /** @brief Cancels the write. The writer deletes its temporary file and then throws OperationCancelled. */
    StopToken stopToken;

    /** @brief Reports progress in rows of the table. It is called on the writing thread. */
    RowProgressCallback progress;

    /**
     * @brief Turns a source path into the same form that AnchorEntry::locator uses, so that anchors can be matched to
     * their sources. The default is the path as UTF-8 text.
     */
    std::function<std::string(const std::filesystem::path &)> canonicalizeSourceLocator;
};

/** @brief What the first line of a bundle holds: the format version, the number of rows and the configuration. */
struct SessionBundleMetadata
{
    std::uint32_t formatVersion = 0;
    std::uint64_t rowCount = 0;
    LogConfiguration configuration;
};

/** @brief Thrown when a bundle has a format version this build does not read. */
class SessionBundleVersionError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/** @brief Thrown when the metadata line of a bundle is missing or invalid. */
class SessionBundleReadError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief Writes the metadata and every row of the table as lines of JSON, in one zstd frame with a checksum.
 *
 * The destination is replaced only after the data has been flushed to disk, so a failed write leaves an existing bundle
 * untouched.
 */
void WriteSessionBundle(
    const LogTable &table,
    const LogConfiguration &configuration,
    const std::filesystem::path &destination,
    const SessionBundleWriteOptions &options = {}
);

/**
 * @brief Reads and checks the first line of a bundle. Throws SessionBundleVersionError or SessionBundleReadError if it
 * is not valid.
 */
[[nodiscard]] SessionBundleMetadata ParseSessionBundleMetadata(std::string_view json);

/**
 * @brief True if the file is zstd data that the decoder accepts. A file that is missing, unreadable or not zstd gives
 * false.
 */
[[nodiscard]] bool LooksLikeSessionBundle(const std::filesystem::path &file) noexcept;

/** @brief The format version this build writes and reads. Any other version is rejected. */
constexpr std::uint32_t SESSION_BUNDLE_FORMAT_VERSION = 1;

/** @brief The most rows a bundle may hold. The writer and the reader both enforce it. */
constexpr std::uint64_t SESSION_BUNDLE_MAX_ROWS = 1'000'000'000ULL;

} // namespace loglib
