#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace loglib
{

/**
 * @brief Controls rotation expansion and already-loaded deduplication.
 *
 * `rotationHistory` is the on/off switch. Callers turn it off for a
 * user preference, a launch opt-out, or a session that does not follow
 * rotated siblings. With it off, inputs are returned in caller order,
 * including duplicates, and nothing is marked auto-added.
 *
 * `suppressKeys` is the append-time dedup set, matched against
 * `CanonicalKeyForPath`. A log-only append passes the keys of locators
 * still visible in the session. A replace, a live-tail open, and a
 * mixed open after a configuration reset pass an empty span: those
 * locators no longer describe visible rows, so they must not hide a
 * file the caller just asked to open. Suppression applies whether or
 * not rotation history is on. The span must outlive the call.
 */
struct SourcePlanOptions
{
    bool rotationHistory = true;
    std::span<const std::string> suppressKeys{};
};

/**
 * @brief One path in load order.
 *
 * `autoAdded` is true only for a numbered or dated sibling that
 * rotation discovery found and the caller did not list. The primary,
 * a residual file, and a caller-listed companion are never auto-added,
 * so a host can count discovered files without treating an explicit
 * selection as an expansion.
 */
struct ResolvedSource
{
    std::filesystem::path path;
    std::string canonicalKey;
    /**
     * @brief Family primary when this entry came from a rotation series.
     * Empty when rotation history is off or the input is residual.
     */
    std::filesystem::path familyPrimary;
    bool autoAdded = false;
};

/**
 * @brief Expands positional paths into the order they should be opened.
 *
 * With rotation history on, paths go through `PartitionAsRotationSeries`.
 * Each family is emitted oldest-first with its primary last. Families
 * are ordered by their earliest input, and inputs that belong to no
 * family follow in caller order. The host opens that sequence front
 * to back; nothing here sorts by timestamp.
 *
 * Two inputs that resolve to the same rotation family are one family.
 * It is emitted once, at the earliest input's position. A later input
 * from that family does not open the family again. Enumeration and the
 * caller's list are merged by canonical key, so a path both sides name
 * appears once. A caller-listed path that enumeration missed is
 * inserted once, before the primary. A missing path is kept as a
 * single residual entry; this function does not check readability.
 *
 * @param paths Positional filesystem paths, in caller order.
 * @param options Rotation and deduplication controls.
 * @return Load order. Empty when @p paths is empty.
 */
[[nodiscard]] std::vector<ResolvedSource> ResolveSourcePlan(
    std::span<const std::filesystem::path> paths, const SourcePlanOptions &options
);

} // namespace loglib
