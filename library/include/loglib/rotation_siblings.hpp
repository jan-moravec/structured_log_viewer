#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace loglib
{

/// One file in a rotated log family. `path` is the on-disk path
/// as it should be handed to the streaming open queue; `canonicalKey`
/// is the deduplication key that matches
/// `LogConfiguration::Source::locatorDedupKeys` (produced by the
/// application-layer canonicaliser; the library computes a
/// lower-cased forward-slash form here so its unit tests can run
/// without a Qt dependency).
///
/// Rotated log detection recognises the two dominant on-disk
/// layouts:
///
///   - Numbered logrotate suffixes: `app.log`, `app.log.1`,
///     `app.log.2.gz`, ... where higher numbers are older.
///   - Dated logrotate suffixes: `app.log-2025-04-28`,
///     `app.log.2025-04-28`, `app.log_2025-04-28`, optionally
///     followed by a compressed-codec extension. Earlier dates
///     are older.
///
/// A returned `RotationSeries` is ordered oldest-first with the
/// primary file appearing last, so callers that want to prepend
/// history in front of the user-selected file can iterate the
/// vector without further sorting.
struct RotatedFile
{
    enum class Origin
    {
        /// The active primary of the series (last entry).
        Primary,
        /// Classified as `<primary>.<N>` (optionally compressed).
        NumberedSuffix,
        /// Classified as a dated variant (tailing or stem-inserted).
        DatedSuffix,
        /// Neither pattern matched, but the caller explicitly listed
        /// this file under the series in `PartitionAsRotationSeries`.
        /// Only produced by the partitioner's union step; the
        /// enumerator never returns this origin. Callers that need
        /// to distinguish "auto-discovered by rotation detection"
        /// from "listed by the user" should treat this value the
        /// same as a caller input, not a rotation companion.
        CallerListed,
    };

    std::filesystem::path path;
    std::string canonicalKey;
    Origin origin = Origin::Primary;
};

/// Family of rotated log files rooted at one primary file.
/// `files` is ordered oldest-first, primary last.
struct RotationSeries
{
    std::filesystem::path primary;
    std::vector<RotatedFile> files;
};

/// Enumerate the rotated companions of @p primary that live in the
/// same directory. Returns a `RotationSeries` containing at minimum
/// the primary itself, ordered oldest-first. Non-existent primaries,
/// unreadable directories, or "nothing matched" cases return the
/// primary alone. All I/O is best-effort and errors are swallowed;
/// call sites should treat this helper as advisory.
[[nodiscard]] RotationSeries EnumerateRotatedSiblings(const std::filesystem::path &primary);

/// Partitioning result for `PartitionAsRotationSeries`.
struct PartitionedSelection
{
    /// One entry per detected family. Each `files` vector is
    /// oldest-first with the primary last, exactly like
    /// `EnumerateRotatedSiblings`.
    std::vector<RotationSeries> series;

    /// Files from the input that did not belong to any family
    /// (their stems have no known rotation form and no sibling was
    /// itself a match). Order preserved from the caller.
    std::vector<std::filesystem::path> residual;
};

/// Split @p paths into rotation families plus a residual bucket for
/// unrelated files. Within each family the ordering matches
/// `EnumerateRotatedSiblings` (oldest-first, primary last). Series
/// appear in the order the earliest-listed member of each family
/// was seen in @p paths so the caller-visible ordering of unrelated
/// inputs is preserved.
///
/// Non-existent paths and paths outside the current filesystem are
/// silently treated as residual, in line with the advisory
/// semantics of the module.
[[nodiscard]] PartitionedSelection PartitionAsRotationSeries(std::span<const std::filesystem::path> paths);

/// Return the lower-cased, forward-slash canonical form of @p path
/// used as the deduplication key in `RotatedFile::canonicalKey`.
/// Exposed for tests and for the app layer to keep its own canonical
/// key in lockstep with the library's.
[[nodiscard]] std::string CanonicalKeyForPath(const std::filesystem::path &path);

} // namespace loglib
