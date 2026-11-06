#include "loglib/parse/parsers/csv_parser.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/file_line_source.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/stream_line_source.hpp"
#include "loglib/parse/internal/classify_bare_scalar.hpp"
#include "loglib/parse/internal/csv_tokenize.hpp"
#include "loglib/parse/internal/line_decoder.hpp"
#include "loglib/parse/internal/probe_line_view.hpp"
#include "loglib/parse/internal/static_parser_pipeline.hpp"
#include "loglib/parse/internal/streaming_parse_loop.hpp"
#include "loglib/parse/log_parser.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

constexpr size_t INITIAL_FIELD_CAPACITY = 16;

constexpr std::string_view UTF8_BOM = "\xEF\xBB\xBF";

using internal::CsvCell;
using internal::TokenizeCsvLine;

/// Strip a leading UTF-8 BOM from @p sv if present.
std::string_view StripBom(std::string_view sv) noexcept
{
    if (sv.starts_with(UTF8_BOM))
    {
        sv.remove_prefix(UTF8_BOM.size());
    }
    return sv;
}

/// Strip a trailing `\r` from @p sv (handles CRLF line endings).
std::string_view StripCr(std::string_view sv) noexcept
{
    if (!sv.empty() && sv.back() == '\r')
    {
        sv.remove_suffix(1);
    }
    return sv;
}

/// Rename duplicate column names in place: first occurrence keeps its
/// name, subsequent ones become `<name>_2`, `<name>_3`, ..., skipping
/// suffixes already taken by another column. Needed because `LogLine`
/// requires unique `KeyId`s per row.
void DeduplicateColumnNames(std::vector<std::string> &names)
{
    std::unordered_set<std::string> seen;
    seen.reserve(names.size());
    for (auto &name : names)
    {
        if (seen.insert(name).second)
        {
            continue;
        }
        const std::string base = name;
        int suffix = 2;
        std::string candidate = fmt::format("{}_{}", base, suffix);
        while (!seen.insert(candidate).second)
        {
            ++suffix;
            candidate = fmt::format("{}_{}", base, suffix);
        }
        name = std::move(candidate);
    }
}

/// Parse a CSV header into `columnKeys`, where `columnKeys[i]` is the
/// `KeyId` for cell `i` in subsequent data rows. internKey turns a column
/// name into its key id. Returns false if the header is empty or has an
/// unterminated quoted cell; duplicates are renamed (see
/// `DeduplicateColumnNames`).
template <class InternKey>
bool ParseHeaderLine(
    std::string_view headerLine, const InternKey &internKey, std::string &headerScratch, std::vector<KeyId> &columnKeys
)
{
    columnKeys.clear();

    if (headerLine.empty())
    {
        return false;
    }

    std::vector<std::string> cellNames;
    cellNames.reserve(INITIAL_FIELD_CAPACITY);

    const bool ok =
        TokenizeCsvLine(headerLine, headerScratch, [&](const CsvCell &cell) { cellNames.emplace_back(cell.value); });
    if (!ok)
    {
        return false;
    }
    if (cellNames.empty())
    {
        return false;
    }

    DeduplicateColumnNames(cellNames);

    columnKeys.reserve(cellNames.size());
    for (auto &name : cellNames)
    {
        columnKeys.push_back(internKey(std::string_view(name)));
    }
    return true;
}

/// Count tokenised cells in @p line; `nullopt` on an unterminated
/// quoted cell. Used only by `IsValid`.
std::optional<size_t> CountCsvCells(std::string_view line, std::string &scratch)
{
    size_t count = 0;
    const bool ok = TokenizeCsvLine(line, scratch, [&](const CsvCell &) { ++count; });
    if (!ok)
    {
        return std::nullopt;
    }
    return count;
}

/// Parse one CSV record into compact values via the header-built
/// `columnKeys` (so no `KeyIndex` / cache is needed per cell).
/// Empty cells are omitted from @p out (absent key == monostate on
/// lookup). `outRaggedExtra` reports cells beyond the header column
/// count; the caller surfaces that as an error.
void ParseCsvLine(
    std::string_view line,
    const std::vector<KeyId> &columnKeys,
    const char *fileBegin,
    size_t fileSize,
    std::string &ownedArena,
    std::string &quotedScratch,
    std::vector<std::pair<KeyId, loglib::CompactLogValue>> &out,
    bool &outUnterminated,
    size_t &outRaggedExtra
)
{
    out.clear();
    // Exact upper bound: extras land in `outRaggedExtra`, not `out`.
    out.reserve(columnKeys.size());
    outUnterminated = false;
    outRaggedExtra = 0;

    size_t columnIndex = 0;
    const bool ok = TokenizeCsvLine(line, quotedScratch, [&](const CsvCell &cell) {
        if (columnIndex >= columnKeys.size())
        {
            ++outRaggedExtra;
            ++columnIndex;
            return;
        }
        const KeyId keyId = columnKeys[columnIndex];
        ++columnIndex;

        if (cell.value.empty() && !cell.wasQuoted)
        {
            return;
        }

        loglib::CompactLogValue compact;
        if (cell.wasQuoted)
        {
            if (cell.fromScratch)
            {
                // Scratch bytes are reused per cell; copy into the arena.
                const uint64_t offset = ownedArena.size();
                ownedArena.append(cell.value.data(), cell.value.size());
                compact = loglib::CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(cell.value.size()));
            }
            else
            {
                compact = internal::MakeStringCompact(cell.value, fileBegin, fileSize, ownedArena);
            }
        }
        else
        {
            compact = internal::ClassifyBareScalar(cell.value, fileBegin, fileSize, ownedArena);
        }

        // Source order, not KeyId order; caller resorts before LogLine ctor.
        out.emplace_back(keyId, compact);
    });

    outUnterminated = !ok;
}

/// `IsValid` false-positive guard: a plausible CSV header has at
/// least 2 cells and at least 2 of them non-empty.
bool HeaderLineLooksLikeCsv(std::string_view line, std::string &scratch)
{
    size_t cellCount = 0;
    size_t nonEmptyCells = 0;
    const bool ok = TokenizeCsvLine(line, scratch, [&](const CsvCell &cell) {
        ++cellCount;
        if (!cell.value.empty())
        {
            ++nonEmptyCells;
        }
    });
    if (!ok)
    {
        return false;
    }
    return cellCount >= 2 && nonEmptyCells >= 2;
}

struct CsvWorkerState
{
    std::string quotedScratch;
};

/** @brief Stage B: decodes the CSV rows of one line-aligned batch into compact lines. */
void DecodeCsvBatch(
    const internal::ByteRange &batch,
    internal::WorkerScratch<CsvWorkerState> &worker,
    KeyIndex &keys,
    FileLineSource &source,
    std::span<const internal::TimeColumnSpec> timeColumns,
    internal::ParsedPipelineBatch &parsed,
    const std::vector<KeyId> &columnKeys,
    std::optional<uint64_t> headerLineOffset
)
{
    parsed.batchIndex = batch.batchIndex;

    const char *cursor = batch.bytesBegin;
    const char *end = batch.bytesEnd;
    const char *fileEnd = batch.fileEnd;
    const char *fileBegin = source.StableBytes().data();

    const auto batchBytes = static_cast<size_t>(end - cursor);
    const size_t estimatedLines = (batchBytes / 64) + 1;
    parsed.lines.reserve(estimatedLines);
    parsed.localLineOffsets.reserve(estimatedLines);

    size_t relativeLineNumber = 1;

    std::vector<std::pair<KeyId, loglib::CompactLogValue>> values;

    while (cursor < end)
    {
        const char *lineStart = cursor;
        const char *newline = static_cast<const char *>(memchr(cursor, '\n', static_cast<size_t>(end - cursor)));
        const char *lineEnd = newline ? newline : end;
        cursor = (newline != nullptr) ? newline + 1 : end;

        std::string_view line(lineStart, static_cast<size_t>(lineEnd - lineStart));
        line = StripCr(line);

        // `RawLine` subtracts 1 for the '\n'; pad by 1 on an unterminated
        // last line so the final byte isn't lost.
        const uint64_t nextOffset = static_cast<uint64_t>(cursor - fileBegin) + (newline == nullptr ? 1u : 0u);
        parsed.localLineOffsets.push_back(nextOffset);

        if (line.empty())
        {
            relativeLineNumber++;
            continue;
        }

        // Skip the header by byte offset (set by the eager parse). This
        // works no matter which Stage B batch contains it -- Stage A
        // ends batches on newline boundaries, so the header line is
        // fully contained in one batch.
        if (headerLineOffset.has_value() && std::cmp_equal(lineStart - fileBegin, *headerLineOffset))
        {
            relativeLineNumber++;
            continue;
        }

        try
        {
            const auto fileSize = static_cast<size_t>(fileEnd - fileBegin);
            bool unterminated = false;
            size_t raggedExtra = 0;
            ParseCsvLine(
                line,
                columnKeys,
                fileBegin,
                fileSize,
                parsed.ownedStringsArena,
                worker.user.quotedScratch,
                values,
                unterminated,
                raggedExtra
            );
            if (unterminated)
            {
                parsed.errors.push_back(
                    internal::ParsedLineError{.relativeLine = relativeLineNumber, .body = "Unterminated quoted value."}
                );
                relativeLineNumber++;
                continue;
            }
            if (raggedExtra != 0)
            {
                parsed.errors.push_back(
                    internal::ParsedLineError{
                        .relativeLine = relativeLineNumber,
                        .body = fmt::format(
                            "Row has {} extra cell(s) beyond the {}-column header.", raggedExtra, columnKeys.size()
                        )
                    }
                );
                relativeLineNumber++;
                continue;
            }
            // Empty `values` is fine: we still emit a row so row count
            // matches visible source lines.

            // LogLine ctor asserts ascending KeyIds; header order may not be.
            std::sort(values.begin(), values.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

            LogLine logLine(std::move(values), keys, source, relativeLineNumber - 1);
            parsed.lines.push_back(std::move(logLine));

            worker.PromoteTimestamps(parsed.lines.back(), timeColumns, std::string_view(parsed.ownedStringsArena));

            values.clear();
        }
        catch (const std::exception &e)
        {
            parsed.errors.push_back(
                internal::ParsedLineError{.relativeLine = relativeLineNumber, .body = std::string(e.what())}
            );
        }

        relativeLineNumber++;
    }

    parsed.totalLineCount = relativeLineNumber - 1;
}

/// CSV decoder for `RunStreamingParseLoop`. Latches the header on the
/// first non-blank line; subsequent lines use the cached `columnKeys`.
class CsvLineDecoder
{
public:
    CsvLineDecoder() = default;

    internal::LineDecodeResult DecodeCompact(
        std::string_view line,
        KeyIndex &keys,
        internal::PerWorkerKeyCache &keyCache,
        std::vector<std::pair<KeyId, loglib::CompactLogValue>> &out,
        std::string &outOwnedArena,
        std::string &errorOut
    )
    {
        out.clear();
        outOwnedArena.clear();
        if (line.empty())
        {
            return internal::LineDecodeResult::Emit;
        }

        if (!mHeaderConsumed)
        {
            const std::string_view headerLine = StripBom(line);
            const auto internKey = [&](std::string_view name) { return internal::InternKeyVia(name, keys, keyCache); };
            if (!ParseHeaderLine(headerLine, internKey, mQuotedScratch, mColumnKeys))
            {
                errorOut = "Invalid CSV header.";
                return internal::LineDecodeResult::Error;
            }
            mHeaderConsumed = true;
            return internal::LineDecodeResult::Skip;
        }

        try
        {
            bool unterminated = false;
            size_t raggedExtra = 0;
            ParseCsvLine(
                line,
                mColumnKeys,
                /* fileBegin = */ nullptr,
                /* fileSize = */ 0,
                outOwnedArena,
                mQuotedScratch,
                out,
                unterminated,
                raggedExtra
            );
            if (unterminated)
            {
                errorOut = "Unterminated quoted value.";
                return internal::LineDecodeResult::Error;
            }
            if (raggedExtra != 0)
            {
                errorOut = fmt::format(
                    "Row has {} extra cell(s) beyond the {}-column header.", raggedExtra, mColumnKeys.size()
                );
                return internal::LineDecodeResult::Error;
            }
            return internal::LineDecodeResult::Emit;
        }
        catch (const std::exception &e)
        {
            errorOut = std::string(e.what());
            return internal::LineDecodeResult::Error;
        }
    }

private:
    bool mHeaderConsumed = false;
    std::vector<KeyId> mColumnKeys;
    std::string mQuotedScratch;
};

static_assert(
    internal::CompactLineDecoder<CsvLineDecoder>, "CsvLineDecoder must satisfy the CompactLineDecoder concept"
);

} // namespace

bool CsvParser::IsValidBytes(std::string_view sniffBuffer) const
{
    // Callers cap `sniffBuffer` at `PROBE_BYTES_BUDGET`, so the
    // cursor bounds are sufficient.
    std::string scratch;
    size_t cursor = 0;

    std::string_view firstView;
    std::string firstStripped;
    bool firstFound = false;
    while (cursor < sniffBuffer.size())
    {
        const internal::ProbeLine probe = internal::NextProbeLine(sniffBuffer, cursor);
        cursor = probe.nextOffset;
        if (probe.line.empty())
        {
            continue;
        }
        firstStripped = std::string(StripBom(probe.line));
        firstView = firstStripped;
        firstFound = true;
        break;
    }
    if (!firstFound)
    {
        return false;
    }

    if (!HeaderLineLooksLikeCsv(firstView, scratch))
    {
        return false;
    }

    const auto headerCellCount = CountCsvCells(firstView, scratch);
    if (!headerCellCount)
    {
        return false;
    }

    // Second non-blank line must have the same cell count.
    while (cursor < sniffBuffer.size())
    {
        const internal::ProbeLine probe = internal::NextProbeLine(sniffBuffer, cursor);
        cursor = probe.nextOffset;
        if (probe.line.empty())
        {
            continue;
        }
        const auto rowCellCount = CountCsvCells(probe.line, scratch);
        if (!rowCellCount)
        {
            return false;
        }
        return *rowCellCount == *headerCellCount;
    }

    return false;
}

void CsvParser::ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    CsvLineDecoder decoder;
    internal::RunStreamingParseLoop(source, decoder, sink, options);
}

void CsvParser::ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    const std::span<const char> bytes = source.StableBytes();
    const size_t batchSize = options.batchSizeBytes != 0 ? options.batchSizeBytes : DEFAULT_PARSE_BATCH_BYTES;

    const char *fileBegin = bytes.data();
    const size_t fileSize = bytes.size();
    const char *fileEnd = (fileBegin != nullptr) ? fileBegin + fileSize : nullptr;

    // Eagerly find and parse the header. Outputs:
    //  * `columnKeys` -- shared `column -> KeyId` table for Stage B.
    //  * `headerLineOffset` -- exact byte offset Stage B uses to skip
    //    the header (handles the case where leading blanks push the
    //    header past batch 0).
    //  * `headerParseFailed` -- short-circuits the data pipeline so a
    //    bad header surfaces one error, not one per row.
    std::vector<KeyId> columnKeys;
    std::optional<uint64_t> headerLineOffset;
    size_t headerLineNumber = 0;
    bool headerParseFailed = false;
    {
        std::string headerScratch;
        const char *cursor = fileBegin;
        bool firstLine = true;
        size_t blankLinesBeforeHeader = 0;
        while (cursor != nullptr && cursor < fileEnd)
        {
            const char *nl = static_cast<const char *>(memchr(cursor, '\n', static_cast<size_t>(fileEnd - cursor)));
            const char *lineEnd = (nl != nullptr) ? nl : fileEnd;
            std::string_view candidate(cursor, static_cast<size_t>(lineEnd - cursor));
            // BOM only valid at the very start of the file.
            if (firstLine)
            {
                candidate = StripBom(candidate);
                firstLine = false;
            }
            candidate = StripCr(candidate);
            if (!candidate.empty())
            {
                headerLineOffset = static_cast<uint64_t>(cursor - fileBegin);
                headerLineNumber = blankLinesBeforeHeader + 1;
                headerParseFailed = !ParseHeaderLine(
                    candidate,
                    [&](std::string_view name) { return sink.Keys().GetOrInsert(name); },
                    headerScratch,
                    columnKeys
                );
                break;
            }
            ++blankLinesBeforeHeader;
            if (nl == nullptr)
            {
                break;
            }
            cursor = nl + 1;
        }
    }

    // Malformed header: emit one error and skip the data pipeline so
    // we don't bury it under one ragged-row error per data line. Still
    // route through `BatchCoalescer::Finish` to keep the sink contract.
    if (headerParseFailed)
    {
        sink.OnStarted();
        KeyIndex &keys = sink.Keys();
        internal::BatchCoalescer coalescer(
            sink, keys, internal::STATIC_BATCH_FLUSH_LINES, internal::STATIC_BATCH_FLUSH_INTERVAL
        );
        coalescer.Pending().errors.push_back(
            ParseError{.firstLine = headerLineNumber, .lastLine = headerLineNumber, .message = "Invalid CSV header."}
        );
        coalescer.Finish(headerLineNumber, /*wasCancelled=*/false);
        return;
    }

    auto stageA = internal::MakeLineAlignedBatcher(fileBegin, fileEnd, batchSize);

    FileLineSource *sourcePtr = &source;
    auto stageB = [sourcePtr, &columnKeys, headerLineOffset](
                      internal::ByteRange token,
                      internal::WorkerScratch<CsvWorkerState> &worker,
                      KeyIndex &keys,
                      std::span<const internal::TimeColumnSpec> timeColumns,
                      internal::ParsedPipelineBatch &parsed
                  ) {
        DecodeCsvBatch(token, worker, keys, *sourcePtr, timeColumns, parsed, columnKeys, headerLineOffset);
    };

    // The header columns are interned up front, so the pipeline announces them as declared keys, even for a column
    // that is empty in every row.
    internal::RunStaticParserPipeline<internal::ByteRange, CsvWorkerState>(
        source, sink, options, stageA, stageB, columnKeys
    );
}

} // namespace loglib
