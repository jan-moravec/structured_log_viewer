#include "loglib/source_plan.hpp"

#include "loglib/rotation_siblings.hpp"

#include <unordered_set>
#include <utility>

namespace loglib
{

namespace
{

[[nodiscard]] bool IsSuppressed(const std::unordered_set<std::string> &keys, const std::string &key)
{
    return keys.contains(key);
}

[[nodiscard]] bool IsAutoDiscoveredSibling(RotatedFile::Origin origin) noexcept
{
    return origin == RotatedFile::Origin::NumberedSuffix || origin == RotatedFile::Origin::DatedSuffix;
}

} // namespace

std::vector<ResolvedSource> ResolveSourcePlan(
    std::span<const std::filesystem::path> paths, const SourcePlanOptions &options
)
{
    std::vector<ResolvedSource> resolved;
    if (paths.empty())
    {
        return resolved;
    }

    const std::unordered_set<std::string> suppressed(options.suppressKeys.begin(), options.suppressKeys.end());

    if (!options.rotationHistory)
    {
        resolved.reserve(paths.size());
        for (const std::filesystem::path &path : paths)
        {
            ResolvedSource entry;
            entry.canonicalKey = CanonicalKeyForPath(path);
            if (IsSuppressed(suppressed, entry.canonicalKey))
            {
                continue;
            }
            entry.path = path;
            resolved.push_back(std::move(entry));
        }
        return resolved;
    }

    std::unordered_set<std::string> callerKeys;
    callerKeys.reserve(paths.size());
    for (const std::filesystem::path &path : paths)
    {
        callerKeys.insert(CanonicalKeyForPath(path));
    }

    const PartitionedSelection partitioned = PartitionAsRotationSeries(paths);
    std::unordered_set<std::string> emitted;
    emitted.reserve(paths.size());

    for (const RotationSeries &series : partitioned.series)
    {
        for (const RotatedFile &file : series.files)
        {
            if (IsSuppressed(suppressed, file.canonicalKey) || !emitted.insert(file.canonicalKey).second)
            {
                continue;
            }
            ResolvedSource entry;
            entry.path = file.path;
            entry.canonicalKey = file.canonicalKey;
            entry.familyPrimary = series.primary;
            entry.autoAdded = IsAutoDiscoveredSibling(file.origin) && !callerKeys.contains(file.canonicalKey);
            resolved.push_back(std::move(entry));
        }
    }

    for (const std::filesystem::path &residual : partitioned.residual)
    {
        std::string key = CanonicalKeyForPath(residual);
        if (IsSuppressed(suppressed, key) || !emitted.insert(key).second)
        {
            continue;
        }
        ResolvedSource entry;
        entry.path = residual;
        entry.canonicalKey = std::move(key);
        resolved.push_back(std::move(entry));
    }

    return resolved;
}

} // namespace loglib
