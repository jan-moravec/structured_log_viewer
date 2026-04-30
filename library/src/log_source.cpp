#include "loglib/log_source.hpp"

#include "loglib/log_file.hpp"

namespace loglib
{

void LogSource::SetRotationCallback(std::function<void()> /*callback*/)
{
    // Default no-op: finite sources never rotate, so the typical override
    // point is `TailingFileSource` (PRD 4.8.7.v).
}

bool LogSource::IsMappedFile() const noexcept
{
    return false;
}

LogFile *LogSource::GetMappedLogFile() noexcept
{
    return nullptr;
}

} // namespace loglib
