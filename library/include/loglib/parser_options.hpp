#pragma once

#include "stop_token.hpp"

#include <cstddef>
#include <memory>
#include <string>

namespace loglib
{

struct LogConfiguration;

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
};

} // namespace loglib
