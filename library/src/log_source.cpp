#include "loglib/log_source.hpp"

#include "loglib/log_file.hpp"

namespace loglib
{

void LogSource::SetRotationCallback(std::function<void()> /*callback*/)
{
    // Default no-op: finite sources never rotate, so the typical override
    // point is `TailingFileSource` (PRD 4.8.7.v).
}

void LogSource::SetStatusCallback(std::function<void(SourceStatus)> /*callback*/)
{
    // Default no-op: finite sources are always `SourceStatus::Running`
    // and never transition (PRD 4.8.8). The typical override point is
    // `TailingFileSource`.
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
