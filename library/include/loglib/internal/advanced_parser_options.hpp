#pragma once

#include "loglib/enum_dictionary.hpp"

#include <cstddef>
#include <cstdint>

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

    /// Per-column distinct-value cap for enum auto-detection.
    /// Forwarded to `LogTable::SetEnumValueCap`; clamped to
    /// `[1, MAX_ENUM_VALUES]`.
    uint16_t enumValueCap = DEFAULT_ENUM_VALUE_CAP;
};

} // namespace loglib::internal
