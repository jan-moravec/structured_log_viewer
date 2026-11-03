#include "loglib/parse/internal/static_parser_pipeline.hpp"

#include "loglib/parse/parser_options.hpp"

#include <algorithm>
#include <thread>

namespace loglib::internal
{

ResolvedPipelineSettings ResolvePipelineSettings(const ParserOptions &options)
{
    ResolvedPipelineSettings out;
    out.effectiveThreads = options.threads != 0
                               ? options.threads
                               : std::min(std::thread::hardware_concurrency(), DEFAULT_MAX_PARSE_THREADS);
    if (out.effectiveThreads == 0)
    {
        out.effectiveThreads = 1;
    }
    out.ntokens = size_t{2} * static_cast<size_t>(out.effectiveThreads);
    return out;
}

} // namespace loglib::internal
