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

ContinuationSpliceOutcome SpliceCrossBatchContinuation(
    LogLine &heldLine, FileLineSource &source, KeyId targetKey, std::string_view leadingContinuationBytes
)
{
    if (targetKey == INVALID_KEY_ID)
    {
        return ContinuationSpliceOutcome::MissingTarget;
    }
    if (leadingContinuationBytes.empty())
    {
        return ContinuationSpliceOutcome::Ok;
    }
    CompactLogValue *slot = heldLine.FindCompactMutable(targetKey);
    if (slot == nullptr)
    {
        return ContinuationSpliceOutcome::MissingTarget;
    }
    if (slot->tag != CompactTag::OwnedString && slot->tag != CompactTag::MmapSlice &&
        slot->tag != CompactTag::Monostate)
    {
        return ContinuationSpliceOutcome::NonStringTarget;
    }

    // Avoid repeatedly copying records that span several batches.
    if (slot->tag == CompactTag::OwnedString)
    {
        std::string extension;
        extension.reserve(1 + leadingContinuationBytes.size());
        extension.push_back('\n');
        extension.append(leadingContinuationBytes);
        if (source.TryExtendOwnedString(slot->payload, slot->aux, extension))
        {
            slot->aux = static_cast<uint32_t>(static_cast<size_t>(slot->aux) + extension.size());
            return ContinuationSpliceOutcome::Ok;
        }
    }

    std::string joined;
    std::string_view current{};
    if (slot->tag != CompactTag::Monostate)
    {
        auto view = heldLine.PeekStringView(*slot);
        if (!view.has_value())
        {
            return ContinuationSpliceOutcome::NonStringTarget;
        }
        current = *view;
    }
    joined.reserve(current.size() + 1 + leadingContinuationBytes.size());
    joined.append(current);
    // A bare key= has no preceding content to separate.
    if (slot->tag != CompactTag::Monostate)
    {
        joined.push_back('\n');
    }
    joined.append(leadingContinuationBytes);

    const uint64_t offset = source.AppendOwnedBytes(joined);
    slot->tag = CompactTag::OwnedString;
    slot->payload = offset;
    slot->aux = static_cast<uint32_t>(joined.size());
    return ContinuationSpliceOutcome::Ok;
}

} // namespace loglib::internal
