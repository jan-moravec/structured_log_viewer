/**
 * @brief Decodes one line-aligned batch of a mapped file with a format's line decoder.
 *
 * DecodeBatch does the work that every format shares when it reads a batch: it splits the bytes into lines, drops the
 * carriage return and the byte order mark, notes where each line ends, counts blank lines, numbers the lines and the
 * errors, collects continuation lines and converts the time columns. A format supplies only a LineDecoder that turns
 * one line into a row. The result has the shape that the delivery stage of the static pipeline expects, so it can join
 * the batches in file order.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/file_line_source.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/parse/internal/decoded_batch.hpp"
#include "loglib/parse/internal/line_decoder.hpp"
#include "loglib/parse/internal/parse_runtime.hpp"
#include "loglib/parse/internal/record_assembler.hpp"
#include "loglib/time/internal/timestamp_promotion.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib::internal
{

/**
 * @brief Decodes the lines of one batch into rows, errors, line offsets and the facts the delivery stage needs.
 *
 * Line numbers are counted from 1 inside the batch, and the delivery stage shifts them to file positions. A record that
 * continues over several lines becomes one row. A record that is still open at the end of the batch is the last row,
 * and the batch says so, because the next batch may continue it. Continuation lines at the start of the batch, before
 * any record, are returned as text for the delivery stage to add to the record it holds from the previous batch.
 */
template <LineDecoder Decoder>
void DecodeBatch(
    const ByteRange &batch,
    Decoder &decoder,
    WorkerScratch &worker,
    KeyIndex &keys,
    FileLineSource &source,
    std::span<const TimeColumnSpec> timeColumns,
    DecodedBatch &parsed
)
{
    parsed.batchIndex = batch.batchIndex;

    const char *cursor = batch.bytesBegin;
    const char *end = batch.bytesEnd;
    const char *fileEnd = batch.fileEnd;
    const std::span<const char> stableBytes = source.StableBytes();
    const char *fileBegin = stableBytes.data();

    if (fileBegin == nullptr || cursor == nullptr || end == nullptr || cursor >= end)
    {
        return;
    }

    const auto batchBytes = static_cast<size_t>(end - cursor);
    const size_t estimatedLines = (batchBytes / 64) + 1;
    parsed.lines.reserve(estimatedLines);
    parsed.localLineOffsets.reserve(estimatedLines);

    LineContext context{
        .keys = keys,
        .keyCache = worker.keyCache,
        .ownedBytes = parsed.ownedStringsArena,
        .stableBytes = stableBytes,
        .bytesAfterLine = 0,
    };
    const std::string_view mappedFile(fileBegin, stableBytes.size());

    size_t relativeLineNumber = 1;
    // The values of the line being decoded, and of the record being sealed, which is the one before it.
    std::vector<CompactLogValue> values;
    std::vector<CompactLogValue> sealedValues;
    std::string lineError;
    // The record that may still get continuation lines. A decoder that never continues has none: its rows are built at
    // once.
    RecordAssembler assembler;
    bool anyRecord = false;
    // Blank lines before the first record of the batch join the continuation text that follows them.
    size_t pendingLeadingBlanks = 0;

    // Turns decoded values into the next row of the batch and converts its time columns.
    const auto addRow = [&](const std::vector<CompactLogValue> &rowValues, size_t headerLine) {
        LogLine logLine(rowValues, parsed.fields, source, headerLine - 1);
        parsed.lines.push_back(std::move(logLine));
        // Inline promotion: the values are still hot. OwnedString payloads still point into the batch arena, which the
        // delivery stage rebases later, so the arena is passed explicitly.
        worker.PromoteTimestamps(parsed.lines.back(), timeColumns, std::string_view(parsed.ownedStringsArena));
    };

    // Joins the continuation text of the open record, makes its row and records its span when it covers several lines.
    const auto sealRecord = [&](bool isBatchTail) {
        if (!assembler.IsOpen())
        {
            return;
        }
        assembler.JoinContinuation(parsed.ownedStringsArena, mappedFile);
        const size_t headerLine = assembler.FirstLine();
        const size_t lastLine = assembler.LastLine();
        const KeyId target = assembler.Target();
        assembler.Close(sealedValues);
        addRow(sealedValues, headerLine);
        if (isBatchTail)
        {
            parsed.lastRecordOpenForContinuation = true;
            parsed.continuationTargetKeyId = target;
            parsed.tailRecordHeaderPhysicalLine = headerLine - 1;
            parsed.tailRecordLastPhysicalLine = lastLine - 1;
        }
        else if (lastLine > headerLine)
        {
            parsed.completedMultiLineSpans.push_back(
                DecodedBatch::MultiLineSpan{.headerPhysicalLine = headerLine - 1, .lastPhysicalLine = lastLine - 1}
            );
        }
        sealedValues.clear();
    };

    const auto continuationDropped = [&](ContinuationSpliceOutcome outcome) {
        parsed.errors.push_back(
            ParsedLineError{
                .relativeLine = relativeLineNumber,
                .body = outcome == ContinuationSpliceOutcome::MissingTarget
                            ? "Continuation lines dropped: target field is not present in the record."
                            : "Continuation lines dropped: target field is not a string.",
            }
        );
    };

    while (cursor < end)
    {
        const char *lineStart = cursor;
        const char *newline = static_cast<const char *>(memchr(cursor, '\n', static_cast<size_t>(end - cursor)));
        const char *lineEnd = newline ? newline : end;
        cursor = (newline != nullptr) ? newline + 1 : end;

        std::string_view line(lineStart, static_cast<size_t>(lineEnd - lineStart));
        if (!line.empty() && line.back() == '\r')
        {
            line.remove_suffix(1);
        }
        if constexpr (StripsBom<Decoder>())
        {
            // Only the first line of the file can start with a byte order mark. Batches end on line boundaries, so that
            // line is the first of batch 0.
            if (lineStart == fileBegin && line.starts_with(UTF8_BYTE_ORDER_MARK))
            {
                line.remove_prefix(UTF8_BYTE_ORDER_MARK.size());
            }
        }

        // RawLine subtracts 1 for the line break, so an unterminated last line gets file size + 1 and keeps its last
        // character.
        const uint64_t nextOffset = static_cast<uint64_t>(cursor - fileBegin) + (newline == nullptr ? 1U : 0U);
        parsed.localLineOffsets.push_back(nextOffset);

        if (line.empty())
        {
            if (!anyRecord)
            {
                ++pendingLeadingBlanks;
            }
            else
            {
                assembler.AddBlank();
            }
            ++relativeLineNumber;
            continue;
        }

        context.bytesAfterLine = static_cast<size_t>(fileEnd - lineEnd);
        const LineDecodeResult result = decoder.Decode(line, context, values, lineError);

        if (result == LineDecodeResult::Error)
        {
            parsed.errors.push_back(ParsedLineError{.relativeLine = relativeLineNumber, .body = lineError});
            lineError.clear();
        }
        else if (result == LineDecodeResult::Continue)
        {
            if (!anyRecord)
            {
                // Include preceding blanks in the physical span of the record held from the previous batch.
                parsed.leadingContinuationLineCount += pendingLeadingBlanks;
                pendingLeadingBlanks = 0;
                if (!parsed.leadingContinuationBytes.empty())
                {
                    parsed.leadingContinuationBytes.push_back('\n');
                }
                parsed.leadingContinuationBytes.append(line.data(), line.size());
                ++parsed.leadingContinuationLineCount;
            }
            else if (!assembler.IsOpen())
            {
                continuationDropped(ContinuationSpliceOutcome::MissingTarget);
            }
            else if (!assembler.AddContinuation(line, relativeLineNumber))
            {
                continuationDropped(assembler.TargetStatus());
            }
        }
        else if (result == LineDecodeResult::Emit)
        {
            pendingLeadingBlanks = 0;
            sealRecord(false);
            anyRecord = true;
            const KeyId target = decoder.ContinuationTarget();
            if (target == INVALID_KEY_ID)
            {
                addRow(values, relativeLineNumber);
                values.clear();
            }
            else
            {
                assembler.Open(values, relativeLineNumber, target);
            }
        }
        // Skip leaves nothing to do: the line only moves the counter on.

        ++relativeLineNumber;
    }

    parsed.totalLineCount = relativeLineNumber - 1;
    sealRecord(true);
}

} // namespace loglib::internal
