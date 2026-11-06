/**
 * @brief Opens a log file from a path and loads it into a table.
 *
 * OpenFileSource turns a path into a FileLineSource that a parser can read. It handles plain files, gzip, bzip2, xz and
 * zstd files, which it unpacks to a temporary copy, and session bundles. LoadFile goes the whole way: it opens the
 * file, detects its format, parses it into a new LogTable the way the GUI and slp do, and finishes type detection.
 *
 * A path is a session bundle when its extension is slvbundle, compared without regard to case. That is the only rule.
 * A bundle must then be a zstd file whose first line is valid bundle metadata, or opening it fails. Any other path is a
 * log, even when its content is zstd.
 *
 * Both functions follow the error model in CONTRIBUTING.md and throw only IoError, FormatError, LimitError or
 * OperationCancelled. They are synchronous and may run on any thread.
 *
 * This is part of the output module. The table side of loading is in table/table_loader.hpp.
 */
#pragma once

#include "loglib/core/file_line_source.hpp"
#include "loglib/core/stop_token.hpp"
#include "loglib/ingest/decompressing_byte_source.hpp"
#include "loglib/output/session_bundle.hpp"
#include "loglib/schema/log_configuration.hpp"
#include "loglib/table/log_table.hpp"
#include "loglib/time/time_zone_context.hpp"

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace loglib
{

/** @brief Settings for OpenFileSource. */
struct OpenFileOptions
{
    /** @brief Called between input chunks while a compressed file is unpacked. May be empty. */
    DecompressingByteSource::ProgressCallback progress;

    /** @brief Cancels unpacking. OpenFileSource then throws OperationCancelled. */
    StopToken stopToken;

    /** @brief The most decompressed bytes allowed. Going past it throws LimitError. Zero turns the cap off. */
    std::size_t maxDecompressedBytes = DecompressingByteSource::DEFAULT_MAX_DECOMPRESSED_BYTES;
};

/** @brief A file that is ready to parse. */
struct OpenedFile
{
    /**
     * @brief The rows' source. It reports the path that was opened, and it keeps an unpacked temporary copy alive until
     * it is destroyed, so rows built from it can be read as long as the source lives.
     */
    std::unique_ptr<FileLineSource> source;

    /** @brief The file that holds the bytes to parse: the unpacked copy, or the path itself for a plain file. */
    std::filesystem::path effectivePath;

    /** @brief The metadata line of a session bundle. Empty for every other file. */
    std::optional<SessionBundleMetadata> bundle;
};

/**
 * @brief Opens a plain, compressed or session bundle file for parsing.
 *
 * Throws IoError when the file is missing or cannot be read or unpacked, FormatError when compressed data is corrupt or
 * a bundle cannot be read, LimitError when the unpacked size passes the cap, and OperationCancelled when the stop token
 * fires. A bundle is recognized by its extension only, see the file comment.
 */
[[nodiscard]] OpenedFile OpenFileSource(const std::filesystem::path &path, const OpenFileOptions &options = {});

/** @brief Settings for LoadFile. */
struct LoadOptions
{
    /**
     * @brief The configuration the table starts with. When unset, a session bundle's own configuration is used, and any
     * other file starts with an empty one.
     */
    std::optional<LogConfiguration> configuration;

    /** @brief How the table reads timestamps that name no zone. UTC by default. */
    TimeZoneContext defaultSourceTimeZone;

    /** @brief The zone in which the table formats timestamps. UTC by default. */
    TimeZoneContext displayTimeZone;

    /** @brief Cancels the load. LoadFile then throws OperationCancelled. */
    StopToken stopToken;
};

/** @brief A loaded table with what the load found. */
struct LoadResult
{
    LogTable table;

    /** @brief Lines and records that did not parse. The rest of the file is in the table. */
    std::vector<ParseError> errors;

    /** @brief Notices about configuration values the library replaced, such as an invalid print format. */
    std::vector<std::string> warnings;

    /** @brief The metadata line when the file was a session bundle. */
    std::optional<SessionBundleMetadata> bundle;
};

/**
 * @brief Opens a file, detects its format and parses it into a new table, as the GUI and slp do.
 *
 * Time columns are converted while parsing and type detection is finished before the call returns. The format comes
 * from the head of the file, except for a session bundle, whose rows are always JSON. Throws what OpenFileSource
 * throws. It also throws IoError for a file that is empty and FormatError for content that no parser accepts.
 */
[[nodiscard]] LoadResult LoadFile(const std::filesystem::path &file, LoadOptions options = {});

} // namespace loglib
