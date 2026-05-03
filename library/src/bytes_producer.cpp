#include "loglib/bytes_producer.hpp"

namespace loglib
{

void BytesProducer::SetRotationCallback(std::function<void()> /*callback*/)
{
    // Default no-op: finite producers never rotate, so the typical override
    // point is `TailingBytesProducer`.
}

void BytesProducer::SetStatusCallback(std::function<void(SourceStatus)> /*callback*/)
{
    // Default no-op: producers that never become unavailable stay
    // `SourceStatus::Running` for their lifetime. The
    // typical override point is `TailingBytesProducer`.
}

} // namespace loglib
