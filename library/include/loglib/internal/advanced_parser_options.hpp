#pragma once

#include "loglib/parser_options.hpp"

#include <cstddef>

namespace loglib::internal
{

/// Tuning knobs for the streaming pipeline. Defaults reproduce the
/// public `Parse(path)` behaviour.
struct AdvancedParserOptions
{
    /// Cap on oneTBB parallelism.
    static constexpr unsigned int DEFAULT_MAX_THREADS = 8;

    /// Stage A batch byte target.
    static constexpr size_t DEFAULT_BATCH_SIZE_BYTES = 1024 * 1024;

    /// `0` means `min(hardware_concurrency, DEFAULT_MAX_THREADS)`.
    unsigned int threads = 0;

    /// Auto-expanded so a line never spans batches.
    size_t batchSizeBytes = DEFAULT_BATCH_SIZE_BYTES;
};

/// Maps public `ParserOptions` pipeline knobs onto this internal struct.
[[nodiscard]] inline AdvancedParserOptions FromParserOptions(const ParserOptions &options) noexcept
{
    AdvancedParserOptions advanced;
    advanced.threads = options.threads;
    if (options.batchSizeBytes != 0)
    {
        advanced.batchSizeBytes = options.batchSizeBytes;
    }
    return advanced;
}

} // namespace loglib::internal
