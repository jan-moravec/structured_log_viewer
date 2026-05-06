#include "loglib/internal/static_parser_pipeline.hpp"

#include <algorithm>
#include <thread>

namespace loglib::internal
{

ResolvedPipelineSettings ResolvePipelineSettings(const AdvancedParserOptions &advanced)
{
    ResolvedPipelineSettings out;
    out.effectiveThreads =
        advanced.threads != 0
            ? advanced.threads
            : std::min(std::thread::hardware_concurrency(), AdvancedParserOptions::DEFAULT_MAX_THREADS);
    if (out.effectiveThreads == 0)
    {
        out.effectiveThreads = 1;
    }
    out.ntokens = size_t{2} * static_cast<size_t>(out.effectiveThreads);
    return out;
}

} // namespace loglib::internal
