/**
 * @brief Lets tests read the progress counters of a TailingBytesProducer.
 *
 * The worker thread of a tailing producer counts the rotations it has seen and the passes it has finished. Tests use
 * the counts to wait for the worker without sleeping. They are not part of the public API.
 *
 * This is part of the ingest module.
 */
#pragma once

#include "loglib/ingest/tailing_bytes_producer.hpp"

#include <cstddef>

namespace loglib::internal
{

/**
 * @brief Reads the counters of a TailingBytesProducer.
 *
 * RotationCount is how many rotations the worker has seen. The rotation callback fires less often when rotations are
 * merged. PollCount is how many passes the worker has finished. A pass checks for rotation and reads what is new, so a
 * test that changed the file and then saw the count rise by two knows the worker has looked at the file since the
 * change.
 */
struct TailingBytesProducerTestAccess
{
    [[nodiscard]] static size_t RotationCount(const TailingBytesProducer &producer) noexcept
    {
        return producer.RotationCount();
    }

    [[nodiscard]] static size_t PollCount(const TailingBytesProducer &producer) noexcept
    {
        return producer.PollCount();
    }
};

} // namespace loglib::internal
