/**
 * @brief What the workers of the static pipeline produce for one batch of a mapped file.
 *
 * The pipeline cuts the file into line-aligned byte ranges, and a worker decodes each range into a DecodedBatch: the
 * rows, their errors and line offsets, and the facts the delivery stage needs to put the batches back in file order,
 * such as continuation text at the start of the batch and whether the last record is still open. This header defines
 * these types and the function that cuts the file into ranges.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/log_line.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace loglib::internal
{

/**
 * @brief An error from decoding one line. relativeLine counts from 1 inside the batch, and the delivery stage turns it
 * into a ParseError with the absolute line number.
 */
struct ParsedLineError
{
    size_t relativeLine = 0;
    std::string body;
};

/**
 * @brief What a worker produces for one batch. The delivery stage puts the batches back in file order before the sink
 * sees them.
 */
struct DecodedBatch
{
    uint64_t batchIndex = 0;
    /**
     * @brief The key ids of the time keys that the worker converts by name, as far as it has met them, sorted. The
     * delivery stage lists them as converted from this batch on.
     */
    std::vector<KeyId> automaticTimeKeys;
    std::vector<LogLine> lines;
    /** @brief The memory the fields of lines are taken from. The delivery stage gives it to the source before it passes
     * lines on.
     */
    FieldArena fields;
    std::vector<uint64_t> localLineOffsets;
    std::vector<ParsedLineError> errors;
    /** @brief The keys this batch met first, in the order the decoder met them. The delivery stage announces them in
     * batch order.
     */
    std::vector<KeyId> firstSeenKeys;
    /**
     * @brief Staging area for the decoded strings of the batch. The delivery stage appends it to the file's arena and
     * shifts the offsets of the lines in one pass.
     */
    std::string ownedStringsArena;
    /**
     * @brief The number of source lines used by the batch: parsed lines, errors and skipped blank lines. It moves
     * the delivery stage's line counter on.
     */
    size_t totalLineCount = 0;
    /**
     * @brief Continuation text at the start of the batch, separated by newlines. The delivery stage adds it to the
     * record held from the previous batch.
     */
    std::string leadingContinuationBytes;
    /**
     * @brief The number of physical lines up to the last leading continuation, counting blank lines before or between
     * them but not blank lines between the last continuation and the next header.
     */
    size_t leadingContinuationLineCount = 0;
    /** @brief True when the delivery stage must hold the last record, because the next batch may continue it. */
    bool lastRecordOpenForContinuation = false;
    /** @brief The string field of the held record that continuation text is added to. */
    KeyId continuationTargetKeyId = INVALID_KEY_ID;
    /**
     * @brief The line, counted inside the batch, where the held record's content ends. Only valid when
     * lastRecordOpenForContinuation is set.
     */
    size_t tailRecordLastPhysicalLine = 0;
    /** @brief The line, counted inside the batch, where the held record starts. */
    size_t tailRecordHeaderPhysicalLine = 0;

    /**
     * @brief The line ranges of multi-line records that are complete inside this batch, counted inside the batch.
     * The delivery stage turns them into absolute line numbers.
     */
    struct MultiLineSpan
    {
        size_t headerPhysicalLine = 0;
        size_t lastPhysicalLine = 0;
    };
    std::vector<MultiLineSpan> completedMultiLineSpans;
};

/** @brief One line-aligned slice of the mapped file handed from the pipeline to a worker. */
struct ByteRange
{
    uint64_t batchIndex = 0;
    const char *bytesBegin = nullptr;
    const char *bytesEnd = nullptr;
    const char *fileEnd = nullptr;
};

/**
 * @brief Returns the function that cuts a mapped file into line-aligned batches.
 *
 * Each call emits the next slice of about batchSize bytes, extended to the end of its last line, until fileEnd. A null
 * range yields no slices.
 */
inline auto MakeLineAlignedBatcher(const char *fileBegin, const char *fileEnd, size_t batchSize)
{
    return [cursor = fileBegin, fileEnd, batchSize, batchIndex = uint64_t{0}](ByteRange &out) mutable -> bool {
        if (cursor == nullptr || cursor >= fileEnd)
        {
            return false;
        }
        const char *batchBegin = cursor;
        const auto remaining = static_cast<size_t>(fileEnd - cursor);
        // Bounded advance: cursor + batchSize past one-past-end is undefined.
        const size_t advance = std::min(batchSize, remaining);
        const char *target = cursor + advance;
        if (advance < remaining)
        {
            const char *newline =
                static_cast<const char *>(memchr(target, '\n', static_cast<size_t>(fileEnd - target)));
            cursor = (newline != nullptr) ? newline + 1 : fileEnd;
        }
        else
        {
            cursor = fileEnd;
        }

        out.batchIndex = batchIndex++;
        out.bytesBegin = batchBegin;
        out.bytesEnd = cursor;
        out.fileEnd = fileEnd;
        return true;
    };
}

} // namespace loglib::internal
