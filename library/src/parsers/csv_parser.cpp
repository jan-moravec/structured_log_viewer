#include "loglib/parsers/csv_parser.hpp"

#include "loglib/file_line_source.hpp"
#include "loglib/internal/advanced_parser_options.hpp"
#include "loglib/internal/classify_bare_scalar.hpp"
#include "loglib/internal/compact_log_value.hpp"
#include "loglib/internal/line_decoder.hpp"
#include "loglib/internal/static_parser_pipeline.hpp"
#include "loglib/internal/streaming_parse_loop.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_processing.hpp"
#include "loglib/stream_line_source.hpp"

#include <fmt/format.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <optional>
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

/// Cap on bytes scanned by `IsValid` for the false-positive guard.
constexpr size_t IS_VALID_PROBE_BYTES = 16 * 1024;

constexpr std::string_view UTF8_BOM = "\xEF\xBB\xBF";

/// One cell from a CSV record.
/// `wasQuoted` disables typed-value detection (matching logfmt).
/// `fromScratch` means `value` points into the decoder's scratch
/// buffer (`""`-unescaped) and must be copied before the next cell
/// overwrites it.
struct CsvCell
{
    std::string_view value;
    bool wasQuoted = false;
    bool fromScratch = false;
};

/// RFC 4180 cell tokenizer. Walks @p line and calls @p emit per cell.
/// Returns false on an unterminated quoted cell (already-emitted cells
/// are kept). @p quotedScratch holds `""`-unescaped bytes across calls.
///
/// Grammar: cells separated by `,`; a leading `"` opens a quoted cell
/// closed by an unescaped `"`, with `""` decoded to a literal `"`; an
/// unquoted cell ends at the next `,` or EOL; a trailing `,` emits one
/// final empty cell.
template <class Emit> bool TokenizeCsvLine(std::string_view line, std::string &quotedScratch, Emit emit)
{
    const char *const data = line.data();
    const size_t end = line.size();
    size_t i = 0;

    while (true)
    {
        if (i < end && data[i] == '"')
        {
            ++i;
            const size_t innerStart = i;
            bool sawEscape = false;
            bool terminated = false;
            while (i < end)
            {
                if (data[i] == '"')
                {
                    if (i + 1 < end && data[i + 1] == '"')
                    {
                        sawEscape = true;
                        i += 2;
                        continue;
                    }
                    terminated = true;
                    break;
                }
                ++i;
            }
            if (!terminated)
            {
                // Includes multi-line cells (unsupported in v1).
                return false;
            }

            const std::string_view rawInner(data + innerStart, i - innerStart);
            ++i;

            CsvCell cell;
            cell.wasQuoted = true;
            if (!sawEscape)
            {
                cell.value = rawInner;
            }
            else
            {
                quotedScratch.clear();
                quotedScratch.reserve(rawInner.size());
                for (size_t j = 0; j < rawInner.size(); ++j)
                {
                    if (rawInner[j] == '"' && j + 1 < rawInner.size() && rawInner[j + 1] == '"')
                    {
                        quotedScratch.push_back('"');
                        ++j;
                    }
                    else
                    {
                        quotedScratch.push_back(rawInner[j]);
                    }
                }
                cell.value = std::string_view(quotedScratch);
                cell.fromScratch = true;
            }
            emit(cell);

            // After a quoted cell, only `,` or EOL is conformant; tolerate
            // stray bytes by skipping to the next `,` (matches logfmt's
            // lax stance).
            if (i >= end)
            {
                return true;
            }
            if (data[i] == ',')
            {
                ++i;
                if (i >= end)
                {
                    const CsvCell empty;
                    emit(empty);
                    return true;
                }
                continue;
            }
            while (i < end && data[i] != ',')
            {
                ++i;
            }
            if (i >= end)
            {
                // e.g. `"a"x` -- the quoted cell is the last one; don't
                // emit a spurious trailing empty.
                return true;
            }
            ++i;
            if (i >= end)
            {
                const CsvCell empty;
                emit(empty);
                return true;
            }
            continue;
        }

        const size_t cellStart = i;
        while (i < end && data[i] != ',')
        {
            ++i;
        }
        CsvCell cell;
        cell.value = std::string_view(data + cellStart, i - cellStart);
        emit(cell);

        if (i >= end)
        {
            return true;
        }
        ++i;
        if (i >= end)
        {
            const CsvCell empty;
            emit(empty);
            return true;
        }
    }
}

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
/// `KeyId` for cell `i` in subsequent data rows. Returns false if the
/// header is empty or has an unterminated quoted cell; duplicates are
/// renamed (see `DeduplicateColumnNames`).
bool ParseHeaderLine(
    std::string_view headerLine, KeyIndex &keys, std::string &headerScratch, std::vector<KeyId> &columnKeys
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
        columnKeys.push_back(keys.GetOrInsert(name));
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
    std::vector<std::pair<KeyId, internal::CompactLogValue>> &out,
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

        internal::CompactLogValue compact;
        if (cell.wasQuoted)
        {
            if (cell.fromScratch)
            {
                // Scratch bytes are reused per cell; copy into the arena.
                const uint64_t offset = ownedArena.size();
                ownedArena.append(cell.value.data(), cell.value.size());
                compact = internal::CompactLogValue::MakeOwnedString(offset, static_cast<uint32_t>(cell.value.size()));
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

/// Stage A token: a contiguous mmap range covering complete lines.
struct CsvByteRange
{
    uint64_t batchIndex = 0;
    const char *bytesBegin = nullptr;
    const char *bytesEnd = nullptr;
    const char *fileEnd = nullptr;
};

void DecodeCsvBatch(
    const CsvByteRange &batch,
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
    const char *fileBegin = source.File().Data();

    const auto batchBytes = static_cast<size_t>(end - cursor);
    const size_t estimatedLines = (batchBytes / 64) + 1;
    parsed.lines.reserve(estimatedLines);
    parsed.localLineOffsets.reserve(estimatedLines);

    size_t relativeLineNumber = 1;

    std::vector<std::pair<KeyId, internal::CompactLogValue>> values;

    while (cursor < end)
    {
        const char *lineStart = cursor;
        const char *newline = static_cast<const char *>(memchr(cursor, '\n', static_cast<size_t>(end - cursor)));
        const char *lineEnd = newline ? newline : end;
        cursor = (newline != nullptr) ? newline + 1 : end;

        std::string_view line(lineStart, static_cast<size_t>(lineEnd - lineStart));
        line = StripCr(line);

        // `GetLine` subtracts 1 for the '\n'; pad by 1 on an unterminated
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
                parsed.errors.push_back(internal::ParsedLineError{
                    .relativeLine = relativeLineNumber,
                    .body = fmt::format(
                        "Row has {} extra cell(s) beyond the {}-column header.", raggedExtra, columnKeys.size()
                    )
                });
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
        internal::PerWorkerKeyCache *keyCache,
        std::vector<std::pair<KeyId, internal::CompactLogValue>> &out,
        std::string &outOwnedArena,
        std::string &errorOut
    )
    {
        // Header-built `columnKeys` already maps every column; the cache
        // is unused. Touch it to silence `/WX` unused-parameter warnings.
        (void)keyCache;

        out.clear();
        outOwnedArena.clear();
        if (line.empty())
        {
            return internal::LineDecodeResult::Emit;
        }

        if (!mHeaderConsumed)
        {
            const std::string_view headerLine = StripBom(line);
            if (!ParseHeaderLine(headerLine, keys, mQuotedScratch, mColumnKeys))
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

/// True iff @p value can serialise unquoted (no `,` / `"` / CR / LF).
bool BareCellIsSafe(std::string_view value) noexcept
{
    return std::ranges::all_of(value, [](char c) { return c != ',' && c != '"' && c != '\r' && c != '\n'; });
}

/// Append @p value as a `"`-quoted cell with RFC-4180 `""` escapes.
void AppendQuotedCell(std::string &out, std::string_view value)
{
    out.push_back('"');
    for (const char c : value)
    {
        if (c == '"')
        {
            out.append("\"\"");
        }
        else
        {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

void AppendCell(std::string &out, std::string_view value)
{
    if (BareCellIsSafe(value))
    {
        out.append(value.data(), value.size());
    }
    else
    {
        AppendQuotedCell(out, value);
    }
}

void AppendValueAsCell(std::string &out, const LogValue &value)
{
    std::visit(
        [&out](const auto &val) {
            using T = std::decay_t<decltype(val)>;
            if constexpr (std::is_same_v<T, std::monostate>)
            {
                // Empty cell.
            }
            else if constexpr (std::is_same_v<T, TimeStamp>)
            {
                AppendCell(out, TimeStampToDateTimeString(val));
            }
            else if constexpr (std::is_same_v<T, std::string_view> || std::is_same_v<T, std::string>)
            {
                AppendCell(out, val);
            }
            else if constexpr (std::is_same_v<T, bool>)
            {
                out.append(val ? "true" : "false");
            }
            else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t>)
            {
                out.append(fmt::format("{}", val));
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                if (std::isfinite(val))
                {
                    out.append(fmt::format("{}", val));
                }
                else
                {
                    // Quote `nan` / `inf` so the round-trip keeps them as
                    // strings rather than reclassifying.
                    AppendCell(out, fmt::format("{}", val));
                }
            }
        },
        value
    );
}

} // namespace

bool CsvParser::IsValid(const std::filesystem::path &file) const
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        return false;
    }

    std::string scratch;
    std::string line;

    std::string_view firstView;
    std::string firstStripped;
    size_t bytesScanned = 0;
    bool firstFound = false;
    while (std::getline(stream, line))
    {
        std::string_view sv(line);
        // Track `\r` so `bytesScanned` matches on-disk bytes; text-mode
        // `getline` may already strip CR, in which case `+1` is correct.
        bool hadCr = false;
        if (!sv.empty() && sv.back() == '\r')
        {
            sv.remove_suffix(1);
            line.pop_back();
            hadCr = true;
        }
        bytesScanned += line.size() + 1 + (hadCr ? 1u : 0u);
        if (sv.empty())
        {
            if (bytesScanned >= IS_VALID_PROBE_BYTES)
            {
                return false;
            }
            continue;
        }
        firstStripped = std::string(StripBom(sv));
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
    while (std::getline(stream, line))
    {
        std::string_view sv(line);
        bool hadCr = false;
        if (!sv.empty() && sv.back() == '\r')
        {
            sv.remove_suffix(1);
            hadCr = true;
        }
        bytesScanned += line.size() + 1 + (hadCr ? 1u : 0u);
        if (sv.empty())
        {
            if (bytesScanned >= IS_VALID_PROBE_BYTES)
            {
                return false;
            }
            continue;
        }
        const auto rowCellCount = CountCsvCells(sv, scratch);
        if (!rowCellCount)
        {
            return false;
        }
        return *rowCellCount == *headerCellCount;
    }

    return false;
}

std::string CsvParser::ToString(const LogLine &line) const
{
    const auto values = line.IndexedValues();
    if (values.empty())
    {
        return {};
    }

    // Values-only, in KeyId order (header-order round-trip is a follow-up).
    std::string out;
    bool first = true;
    for (const auto &entry : values)
    {
        if (!first)
        {
            out.push_back(',');
        }
        first = false;
        AppendValueAsCell(out, entry.second);
    }
    return out;
}

std::string CsvParser::ToString(const LogMap &values)
{
    if (values.empty())
    {
        return {};
    }

    // Lex-by-key for deterministic round-trips (mirrors `LogfmtParser`).
    std::vector<const std::pair<const std::string, LogValue> *> sorted;
    sorted.reserve(values.size());
    for (const auto &kv : values)
    {
        sorted.push_back(&kv);
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto *lhs, const auto *rhs) { return lhs->first < rhs->first; });

    std::string out;
    bool first = true;
    for (const auto *kv : sorted)
    {
        if (!first)
        {
            out.push_back(',');
        }
        first = false;
        AppendValueAsCell(out, kv->second);
    }
    return out;
}

void CsvParser::ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    CsvLineDecoder decoder;
    internal::RunStreamingParseLoop(source, decoder, sink, options);
}

void CsvParser::ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    ParseStreaming(source, sink, options, internal::AdvancedParserOptions{});
}

void CsvParser::ParseStreaming(
    FileLineSource &source, LogParseSink &sink, const ParserOptions &options, internal::AdvancedParserOptions advanced
)
{
    const LogFile &file = source.File();
    const size_t batchSize = advanced.batchSizeBytes != 0 ? advanced.batchSizeBytes
                                                          : internal::AdvancedParserOptions::DEFAULT_BATCH_SIZE_BYTES;

    const char *fileBegin = file.Data();
    const size_t fileSize = file.Size();
    const char *fileEnd = (fileBegin != nullptr) ? fileBegin + fileSize : nullptr;

    // Snapshot key count before the eager header parse so the coalescer
    // surfaces the header columns as `newKeys` even though we intern
    // them up front. Without this the static path would emit no
    // `newKeys` and the `LogTable` would never build any columns.
    const size_t newKeyBaseline = sink.Keys().Size();

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
                headerParseFailed = !ParseHeaderLine(candidate, sink.Keys(), headerScratch, columnKeys);
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
            sink, keys, internal::STATIC_BATCH_FLUSH_LINES, internal::STATIC_BATCH_FLUSH_INTERVAL, newKeyBaseline
        );
        coalescer.Pending().errors.emplace_back(fmt::format("Error on line {}: Invalid CSV header.", headerLineNumber));
        coalescer.Finish(headerLineNumber + 1, /*wasCancelled=*/false);
        return;
    }

    const char *cursor = fileBegin;
    uint64_t batchIndex = 0;

    auto stageA = [cursor, fileEnd, batchSize, batchIndex](CsvByteRange &out) mutable -> bool {
        if (cursor >= fileEnd)
        {
            return false;
        }
        const char *batchBegin = cursor;
        const auto remaining = static_cast<size_t>(fileEnd - cursor);
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

    FileLineSource *sourcePtr = &source;
    auto stageB = [sourcePtr, &columnKeys, headerLineOffset](
                      CsvByteRange token,
                      internal::WorkerScratch<CsvWorkerState> &worker,
                      KeyIndex &keys,
                      std::span<const internal::TimeColumnSpec> timeColumns,
                      internal::ParsedPipelineBatch &parsed
                  ) {
        DecodeCsvBatch(token, worker, keys, *sourcePtr, timeColumns, parsed, columnKeys, headerLineOffset);
    };

    internal::RunStaticParserPipeline<CsvByteRange, CsvWorkerState>(
        source, sink, options, advanced, stageA, stageB, newKeyBaseline
    );
}

} // namespace loglib
