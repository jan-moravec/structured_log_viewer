#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string_view>

namespace log_generator
{

/** @brief How the active output file is rotated. */
enum class RollStrategy
{
    /** Renames `path` to `path.1` and recreates `path`, so the file identity changes. */
    Rename,
    /** Copies `path` to `path.1` and truncates `path` in place, like logrotate's `copytruncate`. */
    CopyTruncate,
    /** Truncates `path` in place without keeping a backup. */
    Truncate,
};

/** @brief When and how the active output file rotates. */
struct RotationOptions
{
    /** Rotates once the file holds at least this many bytes; 0 disables the trigger. */
    std::uint64_t maxBytes = 0;
    /** Rotates once the file holds at least this many lines; 0 disables the trigger. */
    std::uint64_t maxLines = 0;
    RollStrategy strategy = RollStrategy::Rename;
    /** Number of numbered backups `path.1` .. `path.N` to keep. */
    std::size_t keep = 5;

    /** @brief Returns whether any rotation trigger is set. */
    bool Enabled() const noexcept
    {
        return maxBytes != 0 || maxLines != 0;
    }
};

/** @brief Returns the command-line name of @p strategy. */
std::string_view ToString(RollStrategy strategy) noexcept;

/**
 * @brief Moves the closed file at @p path aside according to @p strategy.
 *
 * Existing backups shift up one slot (`path.1` becomes `path.2`, …) and the
 * oldest is removed. `Rename` falls back to copy and remove when the rename
 * fails, and a failed `CopyTruncate` backup is ignored. The caller reopens
 * @p path with truncation afterwards.
 *
 * @param path Active output file; it must be closed.
 * @param strategy Rotation strategy.
 * @param keep Number of backups to keep; 0 discards the content.
 */
void RotateFile(const std::filesystem::path &path, RollStrategy strategy, std::size_t keep);

} // namespace log_generator
