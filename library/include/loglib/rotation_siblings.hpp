#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace loglib
{

/// One file in a rotation family. `path` preserves the path used for
/// opening; `canonicalKey` is the normalized UTF-8 deduplication key.
struct RotatedFile
{
    enum class Origin
    {
        /// Active primary; last when stored in a series.
        Primary,
        /// `<primary>.<N>`, optionally compressed.
        NumberedSuffix,
        /// Dated suffix or stem-inserted date.
        DatedSuffix,
        /// Explicit input added to a family but missed by enumeration.
        CallerListed,
    };

    std::filesystem::path path;
    std::string canonicalKey;
    Origin origin = Origin::Primary;
};

/// Classified files are oldest-first within each naming convention.
/// Unranked caller inputs precede the primary, which is always last.
struct RotationSeries
{
    std::filesystem::path primary;
    std::vector<RotatedFile> files;
};

/// Enumerates numbered and dated companions in @p primary's directory.
/// Numbered suffixes sort by descending number and dated suffixes by date.
/// Mixed families compare days since 1970 with `1,000,000,000 - N`;
/// equal ranks have unspecified order.
/// The primary is always present and last. Directory I/O is advisory:
/// errors may return partial results, and no matches yield the primary alone.
[[nodiscard]] RotationSeries EnumerateRotatedSiblings(const std::filesystem::path &primary);

struct PartitionedSelection
{
    /// Detected families in caller order.
    std::vector<RotationSeries> series;

    /// Inputs not assigned to a family, in caller order and deduplicated by key.
    std::vector<std::filesystem::path> residual;
};

/// Splits @p paths into rotation families and residual inputs. Family
/// ordering matches `EnumerateRotatedSiblings`; families are ordered by
/// their earliest input member. Filesystem checks are best-effort.
[[nodiscard]] PartitionedSelection PartitionAsRotationSeries(std::span<const std::filesystem::path> paths);

/// Returns a best-effort absolute, lexically normalized UTF-8 key with
/// forward slashes. ASCII letters are lower-cased only on Windows.
[[nodiscard]] std::string CanonicalKeyForPath(const std::filesystem::path &path);

} // namespace loglib
