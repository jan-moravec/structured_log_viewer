#include <log_generator/rotation.hpp>

#include <string>
#include <system_error>

namespace log_generator
{

namespace
{

// `<base>.<n>`, the logrotate naming convention (`app.log.1`, `app.log.2`, …).
std::filesystem::path NumberedBackup(const std::filesystem::path &basePath, std::size_t n)
{
    std::filesystem::path p = basePath;
    p += "." + std::to_string(n);
    return p;
}

// Shift backups up one slot and drop the oldest, freeing `path.1`. Errors
// are swallowed because a missing backup slot must not fail the rotation.
void ShiftBackups(const std::filesystem::path &basePath, std::size_t keep)
{
    std::error_code ec;
    std::filesystem::remove(NumberedBackup(basePath, keep), ec);
    for (std::size_t i = keep; i >= 2; --i)
    {
        const auto from = NumberedBackup(basePath, i - 1);
        if (std::filesystem::exists(from, ec))
        {
            std::filesystem::rename(from, NumberedBackup(basePath, i), ec);
        }
    }
}

} // namespace

std::string_view ToString(RollStrategy strategy) noexcept
{
    switch (strategy)
    {
    case RollStrategy::Rename:
        return "rename";
    case RollStrategy::CopyTruncate:
        return "copytruncate";
    case RollStrategy::Truncate:
        return "truncate";
    }
    return "rename";
}

void RotateFile(const std::filesystem::path &path, RollStrategy strategy, std::size_t keep)
{
    switch (strategy)
    {
    case RollStrategy::Rename:
    {
        std::error_code ec;
        if (keep == 0)
        {
            std::filesystem::remove(path, ec);
            return;
        }
        ShiftBackups(path, keep);
        std::filesystem::rename(path, NumberedBackup(path, 1), ec);
        if (ec)
        {
            // Some filesystems (Windows with another reader) block renaming
            // an open file; copy and remove keeps the outcome the same.
            std::filesystem::copy_file(
                path, NumberedBackup(path, 1), std::filesystem::copy_options::overwrite_existing
            );
            std::filesystem::remove(path);
        }
        return;
    }
    case RollStrategy::CopyTruncate:
    {
        if (keep > 0)
        {
            ShiftBackups(path, keep);
            std::error_code ec;
            std::filesystem::copy_file(
                path, NumberedBackup(path, 1), std::filesystem::copy_options::overwrite_existing, ec
            );
        }
        return;
    }
    case RollStrategy::Truncate:
        return;
    }
}

} // namespace log_generator
