#pragma once

#include <cstddef>

namespace loglib::internal
{

/// Tuning knobs for the streaming pipeline. A default-constructed instance
/// reproduces the public `Parse(path)` behaviour; tests and benchmarks use
/// it to pin determinism (single thread, smaller batches).
struct AdvancedParserOptions
{
    /// Cap on per-process oneTBB parallelism so the parser does not monopolise
    /// large-core machines when the caller does not pin threads.
    static constexpr unsigned int DEFAULT_MAX_THREADS = 8;

    /// Stage A batch byte target. Picked to keep each Stage B worker's
    /// working set warm in L2 while bounding in-flight memory.
    static constexpr size_t DEFAULT_BATCH_SIZE_BYTES = 1024 * 1024;

    /// `0` means `min(hardware_concurrency, DEFAULT_MAX_THREADS)`.
    unsigned int threads = 0;

    /// Values smaller than a single line auto-expand so a line never spans batches.
    size_t batchSizeBytes = DEFAULT_BATCH_SIZE_BYTES;
};

} // namespace loglib::internal
