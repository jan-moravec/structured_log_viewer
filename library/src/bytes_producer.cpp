#include "loglib/bytes_producer.hpp"

namespace loglib
{

void BytesProducer::SetRotationCallback(const std::function<void()> & /*callback*/)
{
    // No-op: finite producers never rotate.
}

void BytesProducer::SetStatusCallback(const std::function<void(SourceStatus)> & /*callback*/)
{
    // No-op: producers that never become unavailable stay `Running`.
}

} // namespace loglib
