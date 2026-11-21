/**
 * @brief Lets tests build a StdinBytesProducer on a pipe of their own.
 *
 * The public constructors read the process's standard input. A test needs to feed the producer from a pipe it controls,
 * so this header gives it a way to hand the read end of a pipe to the producer. It is only for library tests.
 *
 * This is part of the ingest module.
 */
#pragma once

#include "loglib/ingest/stdin_bytes_producer.hpp"

#include <memory>

namespace loglib::internal
{

/**
 * @brief Builds a StdinBytesProducer that reads from a handle given by the caller.
 *
 * Tests create a pipe and pass its read end to Create, and the producer owns and closes that end. On Windows the handle
 * is a HANDLE cast to a pointer. Elsewhere it is a file descriptor converted to an integer and then to a pointer.
 */
struct StdinBytesProducerTestAccess
{
    [[nodiscard]] static std::unique_ptr<StdinBytesProducer> Create(
        void *opaqueHandle, StdinBytesProducer::Options options
    );
};

} // namespace loglib::internal
