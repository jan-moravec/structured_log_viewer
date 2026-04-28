#include "parser_pipeline.hpp"

#include <algorithm>
#include <thread>

namespace loglib::detail
{

ResolvedPipelineSettings ResolvePipelineSettings(const internal::AdvancedParserOptions &advanced)
{
    ResolvedPipelineSettings out;
    out.effectiveThreads =
        advanced.threads != 0
            ? advanced.threads
            : std::min(std::thread::hardware_concurrency(), internal::AdvancedParserOptions::kDefaultMaxThreads);
    if (out.effectiveThreads == 0)
    {
        out.effectiveThreads = 1;
    }
    out.ntokens = advanced.ntokens != 0 ? advanced.ntokens : static_cast<size_t>(2 * out.effectiveThreads);
    return out;
}

} // namespace loglib::detail
