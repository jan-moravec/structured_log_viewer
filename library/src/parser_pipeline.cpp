#include "parser_pipeline.hpp"

#include <algorithm>
#include <thread>

namespace loglib::detail
{

ResolvedPipelineSettings ResolvePipelineSettings(const PipelineHarnessOptions &opts)
{
    ResolvedPipelineSettings out;
    out.effectiveThreads = opts.threads != 0
                               ? opts.threads
                               : std::min(std::thread::hardware_concurrency(),
                                          static_cast<unsigned int>(PipelineHarnessOptions::kDefaultMaxThreads));
    if (out.effectiveThreads == 0)
    {
        out.effectiveThreads = 1;
    }
    out.ntokens = opts.ntokens != 0 ? opts.ntokens : static_cast<size_t>(2 * out.effectiveThreads);
    out.batchSizeBytes = opts.batchSizeBytes != 0 ? opts.batchSizeBytes
                                                  : PipelineHarnessOptions::kDefaultBatchSizeBytes;
    return out;
}

} // namespace loglib::detail
