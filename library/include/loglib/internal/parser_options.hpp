#pragma once

#include <chrono>
#include <cstddef>

namespace loglib
{

/// Per-stage timing telemetry for one streaming parse. Stage B is parallel, so
/// `stageBCpuTotal` is summed across workers.
struct StageTimings
{
    std::chrono::nanoseconds wallClockTotal{0};
    std::chrono::nanoseconds stageACpuTotal{0};
    std::chrono::nanoseconds stageBCpuTotal{0};
    std::chrono::nanoseconds stageCCpuTotal{0};
    std::chrono::nanoseconds sinkTotal{0};
    unsigned int effectiveThreads = 1;
    size_t stageABatches = 0;
    size_t stageBBatches = 0;
    size_t stageCBatches = 0;
};

namespace internal
{

/// Tuning knobs for the streaming pipeline. A default-constructed instance
/// reproduces the public `Parse(path)` behaviour.
struct AdvancedParserOptions
{
    /// Cap on per-process oneTBB parallelism so the parser does not monopolise
    /// large-core machines when the caller does not pin threads.
    static constexpr unsigned int kDefaultMaxThreads = 8;

    /// Stage A batch byte target. Picked to keep each Stage B worker's
    /// working set warm in L2 while bounding in-flight memory.
    static constexpr size_t kDefaultBatchSizeBytes = 1024 * 1024;

    /// `0` means `min(hardware_concurrency, kDefaultMaxThreads)`.
    unsigned int threads = 0;

    /// Values smaller than a single line auto-expand so a line never spans batches.
    size_t batchSizeBytes = kDefaultBatchSizeBytes;

    /// Pipeline depth in tokens. `0` defaults to `2 * effectiveThreads`.
    size_t ntokens = 0;

    /// Per-worker key-string -> KeyId cache. On by default.
    bool useThreadLocalKeyCache = true;

    /// Optional caller-owned timings sink, written once before `OnFinished`.
    /// `mutable` so the option can sit behind a `const` benchmark wrapper.
    mutable StageTimings *timings = nullptr;
};

} // namespace internal
} // namespace loglib
