#pragma once

#include "loglib/core/bytes_producer.hpp"
#include "loglib/core/stop_token.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace loglib
{

struct LogConfiguration;

/** @brief Thread cap used when ParserOptions::threads is 0, applied on top of the hardware concurrency. */
inline constexpr unsigned int DEFAULT_MAX_PARSE_THREADS = 8;

/** @brief Stage A batch size used when ParserOptions::batchSizeBytes is 0. */
inline constexpr std::size_t DEFAULT_PARSE_BATCH_BYTES = std::size_t{1024} * 1024;

/**
 * @brief Public options for `LogParser::ParseStreaming`.
 *
 * `threads` and `batchSizeBytes` tune the static-file TBB path.
 * `0` selects the library defaults (`min(hardware_concurrency, 8)`
 * threads and a 1 MiB Stage A batch). `configuration` is a nullable
 * shared pointer; this header forward-declares `LogConfiguration` so
 * parse options do not pull in the full configuration definition.
 */
struct ParserOptions
{
    StopToken stopToken{};
    /** @brief Optional immutable parse configuration. May be null. */
    std::shared_ptr<const LogConfiguration> configuration;
    /**
     * @brief Enables logfmt continuation folding.
     *
     * When enabled, Logfmt treats space- or tab-prefixed lines as
     * continuations of the preceding record's last source-order field.
     * Has no effect on other parsers.
     */
    bool multilineLogfmt = true;

    /**
     * @brief Bytes already consumed that must be reprocessed first.
     *
     * Used by `AutoDetectParser` and the stdin path to hand bytes
     * drained during format detection back to the resolved parser
     * without swapping the producer. Non-empty only on the streaming
     * (`StreamLineSource`) path; the static-file path asserts the
     * carry is empty. Empty by default.
     */
    std::string initialCarry;

    /**
     * @brief TBB worker cap for the static-file pipeline.
     *
     * `0` means `min(hardware_concurrency, 8)`.
     */
    unsigned int threads = 0;

    /**
     * @brief Stage A batch size in bytes.
     *
     * `0` means 1 MiB. The pipeline auto-expands so a line never
     * spans batches.
     */
    std::size_t batchSizeBytes = 0;

    /**
     * @brief Longest line and longest multi-line record the live-stream loop
     * buffers.
     *
     * A longer line is dropped with one error and parsing resumes after its
     * newline. Further continuation lines of a longer record are dropped
     * and counted in one error. The record itself is kept.
     */
    std::size_t maxLineBytes = DEFAULT_MAX_LINE_BYTES;
};

} // namespace loglib
