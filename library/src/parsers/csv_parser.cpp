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
#include <string>
#include <string_view>
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

/// One cell from a single CSV record.
///
/// `wasQuoted`: the cell was wrapped in `"`-quotes on disk, so it
///   bypasses typed-value detection (matches logfmt's rule that
///   quoted values stay strings).
/// `fromScratch`: cell bytes live in the decoder's `quotedScratch`
///   buffer (because they were unescaped from `""`-escapes), not in
///   the source mmap / carry buffer, so they must be copied into the
///   arena before the next cell overwrites the scratch.
struct CsvCell
{
    std::string_view value;
    bool wasQuoted = false;
    bool fromScratch = false;
};

/// State machine for an RFC-4180 record line. Walks @p line and emits
/// each cell via @p emit. Returns false on an unterminated quoted cell;
/// cells emitted before that point are kept. @p quotedScratch is reused
/// across calls to hold unescaped (`""` -> `"`) quoted bytes.
///
/// The grammar:
///   - cells are separated by `,`
///   - a cell that starts with `"` is quoted: everything up to the
///     matching unescaped `"` is its content; `""` inside is a literal
///     `"`; any other `\\`-style escape passes through verbatim (RFC
///     4180 has no other escape forms)
///   - an unquoted cell ends at the next `,` or end-of-line
///   - a trailing `,` produces an empty cell; an empty line still
///     produces no cells (caller skips empty lines)
template <class Emit> bool TokenizeCsvLine(std::string_view line, std::string &quotedScratch, Emit emit)
{
    const char *const data = line.data();
    const size_t end = line.size();
    size_t i = 0;

    while (true)
    {
        // Each iteration parses one cell, starting at `i`. A leading `"`
        // (after any leading whitespace? -- no, RFC 4180 is strict) means
        // the cell is quoted.
        if (i < end && data[i] == '"')
        {
            ++i; // consume opening '"'
            const size_t innerStart = i;
            bool sawEscape = false;
            bool terminated = false;
            while (i < end)
            {
                if (data[i] == '"')
                {
                    if (i + 1 < end && data[i + 1] == '"')
                    {
                        // `""` -> literal `"`. Walk past both bytes.
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
                // Unterminated quote (multi-line cells fall here too).
                return false;
            }

            const std::string_view rawInner(data + innerStart, i - innerStart);
            ++i; // consume closing '"'

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

            // After a quoted cell, expect either end-of-line or `,`.
            // Any other byte (e.g. `foo"bar,baz`) is non-conforming;
            // be tolerant and skip up to the next `,` rather than
            // surfacing a hard error -- matches the lax stance the
            // logfmt parser takes for stray bytes.
            if (i >= end)
            {
                return true;
            }
            if (data[i] == ',')
            {
                ++i;
                // A trailing comma -> one more empty cell. Fall
                // through to the unquoted branch with an empty span
                // by letting the loop re-enter.
                if (i >= end)
                {
                    CsvCell empty;
                    emit(empty);
                    return true;
                }
                continue;
            }
            // Garbage after the closing quote; skip to the next
            // comma (or end). The cell we already emitted stands.
            while (i < end && data[i] != ',')
            {
                ++i;
            }
            if (i < end)
            {
                ++i;
                if (i >= end)
                {
                    CsvCell empty;
                    emit(empty);
                    return true;
                }
            }
            continue;
        }

        // Unquoted cell: read until next `,` or end-of-line.
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
        ++i; // consume `,`
        if (i >= end)
        {
            // Trailing `,` -> one more empty cell.
            CsvCell empty;
            emit(empty);
            return true;
        }
    }
}

/// Strip a leading UTF-8 BOM from @p sv if present.
std::string_view StripBom(std::string_view sv) noexcept
{
    if (sv.size() >= UTF8_BOM.size() && sv.substr(0, UTF8_BOM.size()) == UTF8_BOM)
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

/// Parse a CSV header line into a column key list. Returns false if
/// the header is empty / has fewer than 2 cells / contains an
/// unterminated quoted cell. The resulting `columnKeys[i]` is the
/// `KeyId` for cell `i` in any subsequent data row.
bool ParseHeaderLine(
    std::string_view headerLine,
    KeyIndex &keys,
    std::string &headerScratch,
    std::vector<KeyId> &columnKeys
)
{
    columnKeys.clear();

    // Empty header -> reject.
    if (headerLine.empty())
    {
        return false;
    }

    std::vector<std::string> cellStorage;
    cellStorage.reserve(INITIAL_FIELD_CAPACITY);

    const bool ok = TokenizeCsvLine(headerLine, headerScratch, [&](const CsvCell &cell) {
        cellStorage.emplace_back(cell.value);
    });
    if (!ok)
    {
        return false;
    }
    if (cellStorage.empty())
    {
        return false;
    }
    columnKeys.reserve(cellStorage.size());
    for (auto &name : cellStorage)
    {
        columnKeys.push_back(keys.GetOrInsert(name));
    }
    return true;
}

/// Count tokenised cells in @p line. Returns `nullopt` on an
/// unterminated quoted cell (the caller treats that as "not a valid
/// CSV record"). Used only by `IsValid`.
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

/// Parse one CSV record into compact values. `columnKeys` is the
/// `column index -> KeyId` map produced by `ParseHeaderLine`.
/// `outRaggedExtra` is set to the number of trailing cells beyond the
/// header's column count; the caller turns that into an error if non-zero.
///
/// Empty cells (`a,,c` or trailing missing cells) are **omitted** from
/// @p out -- an absent key materialises as monostate on lookup, and
/// skipping the slot saves 16 B per empty cell on wide CSV rows.
///
/// Note: `keys` / `keyCache` are not parameters here because every
/// column's `KeyId` was interned once when the header was parsed
/// (see `ParseHeaderLine`); cell processing only needs the
/// header-built `columnKeys` lookup.
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
    out.reserve(std::min(columnKeys.size(), INITIAL_FIELD_CAPACITY));
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
            // Empty bare cell -> omit (see header docstring).
            return;
        }

        internal::CompactLogValue compact;
        if (cell.wasQuoted)
        {
            if (cell.fromScratch)
            {
                const uint64_t offset = ownedArena.size();
                ownedArena.append(cell.value.data(), cell.value.size());
                compact = internal::CompactLogValue::MakeOwnedString(
                    offset, static_cast<uint32_t>(cell.value.size())
                );
            }
            else
            {
                compact = internal::MakeStringCompact(cell.value, fileBegin, fileSize, ownedArena);
            }
        }
        else
        {
            // `cell.value` is non-empty here (the empty-bare branch
            // returned above), so `ClassifyBareScalar` will not produce
            // monostate -- result is always one of bool / int64 / uint64
            // / double / string.
            compact = internal::ClassifyBareScalar(cell.value, fileBegin, fileSize, ownedArena);
        }

        // `out` is built in source order (which is also KeyId order
        // *iff* the header columns happen to be sorted) -- the caller
        // resorts before constructing the `LogLine`.
        out.emplace_back(keyId, compact);
    });

    outUnterminated = !ok;
}

/// True if @p line looks like a plausible CSV header. Used by `IsValid`
/// for the false-positive guard. The line is **already tokenised**
/// here -- we just need a quick gate: at least one non-empty cell name
/// AND total cell count >= 2 (matches the plan's "ge 2 non-empty cells").
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

/// CSV-specific per-worker scratch.
struct CsvWorkerState
{
    std::string quotedScratch;
};

/// Stage A token: a contiguous byte range of the mmap covering complete lines.
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
    const std::vector<KeyId> &columnKeys
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

    // In batch 0, swallow the first non-blank line (it's the header).
    // The line still registers its offset and bumps `relativeLineNumber`
    // so `LogFile::GetLine(lineId)` stays aligned for every subsequent
    // data row (`GetLine(1)` -> first data row, etc.).
    bool headerToSkip = (batch.batchIndex == 0);

    std::vector<std::pair<KeyId, internal::CompactLogValue>> values;

    while (cursor < end)
    {
        const char *lineStart = cursor;
        const char *newline = static_cast<const char *>(memchr(cursor, '\n', static_cast<size_t>(end - cursor)));
        const char *lineEnd = newline ? newline : end;
        cursor = (newline != nullptr) ? newline + 1 : end;

        std::string_view line(lineStart, static_cast<size_t>(lineEnd - lineStart));
        line = StripCr(line);

        // `GetLine` subtracts 1 (the '\n'); for an unterminated last
        // line push `fileSize + 1` so we don't lose the final char.
        const uint64_t nextOffset = static_cast<uint64_t>(cursor - fileBegin) + (newline == nullptr ? 1u : 0u);
        parsed.localLineOffsets.push_back(nextOffset);

        if (line.empty())
        {
            relativeLineNumber++;
            continue;
        }

        if (headerToSkip)
        {
            // First non-blank line in batch 0 is the header. Already
            // parsed eagerly by `CsvParser::ParseStreaming` to build
            // `columnKeys`; here we just swallow it (no LogLine, no
            // error) while still consuming its line slot.
            headerToSkip = false;
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
                        "Row has {} extra cell(s) beyond the {}-column header.",
                        raggedExtra,
                        columnKeys.size()
                    )
                });
                relativeLineNumber++;
                continue;
            }
            if (values.empty())
            {
                // Every cell was empty -> nothing stored. Still emit
                // a `LogLine` with no fields so the row count matches
                // the visible source lines.
            }

            // `LogLine` ctor's debug `is_sorted` assertion requires
            // ascending KeyIds; header order may not be sorted.
            std::sort(values.begin(), values.end(), [](const auto &a, const auto &b) {
                return a.first < b.first;
            });

            LogLine logLine(std::move(values), keys, source, relativeLineNumber - 1);
            parsed.lines.push_back(std::move(logLine));

            worker.PromoteTimestamps(parsed.lines.back(), timeColumns, std::string_view(parsed.ownedStringsArena));

            // Reset the moved-from vector for the next line.
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

/// CSV record decoder for `RunStreamingParseLoop`. Owns the per-line
/// scratch + a header latch with the stashed `columnKeys`. Satisfies
/// `CompactLineDecoder`.
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
        // `keyCache` is irrelevant once the header is consumed:
        // every cell's KeyId comes from the header-built table.
        // Touch it so MSVC doesn't flag the unused parameter under
        // `/WX`. (The concept requires the parameter.)
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

/// True if @p value can serialise as a bare CSV cell (no `,`, no `"`,
/// no `\r`, no `\n`). Bare cells round-trip without quoting; everything
/// else gets RFC-4180-quoted.
bool BareCellIsSafe(std::string_view value) noexcept
{
    return std::ranges::all_of(value, [](char c) {
        return c != ',' && c != '"' && c != '\r' && c != '\n';
    });
}

/// Append @p value as a `"`-quoted CSV cell with RFC-4180 `""`
/// escapes for embedded quote characters.
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
            else if constexpr (std::is_same_v<T, std::string_view>)
            {
                AppendCell(out, val);
            }
            else if constexpr (std::is_same_v<T, std::string>)
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
                // Non-finite doubles (`nan`, `inf`, `-inf`) print as
                // bare alphabetic tokens. Quote them so the round-trip
                // preserves a recognisable string rather than letting
                // the reader silently classify them as something else.
                if (std::isfinite(val))
                {
                    out.append(fmt::format("{}", val));
                }
                else
                {
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

    // Find the first non-blank line. Strip a leading BOM if present.
    std::string_view firstView;
    std::string firstStripped;
    size_t bytesScanned = 0;
    bool firstFound = false;
    while (std::getline(stream, line))
    {
        std::string_view sv(line);
        if (!sv.empty() && sv.back() == '\r')
        {
            sv.remove_suffix(1);
            line.pop_back();
        }
        bytesScanned += line.size() + 1;
        if (sv.empty())
        {
            if (bytesScanned >= IS_VALID_PROBE_BYTES)
            {
                return false;
            }
            continue;
        }
        // BOM strip on the first non-blank line.
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

    // Count cells in the header.
    const auto headerCellCount = CountCsvCells(firstView, scratch);
    if (!headerCellCount)
    {
        return false;
    }

    // Find the next non-blank line; require the same tokenised cell count.
    while (std::getline(stream, line))
    {
        std::string_view sv(line);
        if (!sv.empty() && sv.back() == '\r')
        {
            sv.remove_suffix(1);
        }
        bytesScanned += line.size() + 1;
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

    // Header but no second non-blank line -> reject (false-positive guard).
    return false;
}

std::string CsvParser::ToString(const LogLine &line) const
{
    const auto values = line.IndexedValues();
    if (values.empty())
    {
        return {};
    }

    const auto &keys = line.Keys();
    std::string out;
    bool first = true;
    for (const auto &entry : values)
    {
        if (!first)
        {
            out.push_back(',');
        }
        first = false;
        const std::string_view key = keys.KeyOf(entry.first);
        (void)key; // Header order is not preserved in v1 (see header docstring).
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

    // Lexicographic by key for round-trip determinism (mirrors
    // `LogfmtParser::ToString(LogMap)`).
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

    // Eagerly parse the header off the mmap so every Stage B worker
    // sees the same read-only `column index -> KeyId` table. This is
    // schema-only -- the byte cursor stays at `fileBegin` (offset 0)
    // so the header line still registers its offset slot via the
    // normal Stage B path and `LogFile::GetLine` stays aligned.
    std::vector<KeyId> columnKeys;
    {
        std::string headerScratch;
        if (fileBegin != nullptr && fileSize > 0)
        {
            // Walk to the first non-blank line; strip BOM on the way.
            const char *cursor = fileBegin;
            std::string_view stripped(fileBegin, fileSize);
            stripped = StripBom(stripped);
            // Adjust cursor for any stripped BOM.
            cursor = stripped.data();
            const char *bomEnd = stripped.data() + stripped.size();
            while (cursor < bomEnd)
            {
                const char *nl = static_cast<const char *>(memchr(cursor, '\n', static_cast<size_t>(bomEnd - cursor)));
                const char *lineEnd = (nl != nullptr) ? nl : bomEnd;
                std::string_view candidate(cursor, static_cast<size_t>(lineEnd - cursor));
                candidate = StripCr(candidate);
                if (!candidate.empty())
                {
                    (void)ParseHeaderLine(candidate, sink.Keys(), headerScratch, columnKeys);
                    break;
                }
                if (nl == nullptr)
                {
                    break;
                }
                cursor = nl + 1;
            }
        }
    }

    // If the header parse failed (empty file, malformed) we still go
    // through the pipeline; every data row will then be a no-column
    // error, surfaced as `Row has N extra cell(s) beyond the 0-column
    // header.` That matches the contract `IsValid` rejected on.

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
    auto stageB = [sourcePtr, &columnKeys](
                      CsvByteRange token,
                      internal::WorkerScratch<CsvWorkerState> &worker,
                      KeyIndex &keys,
                      std::span<const internal::TimeColumnSpec> timeColumns,
                      internal::ParsedPipelineBatch &parsed
                  ) { DecodeCsvBatch(token, worker, keys, *sourcePtr, timeColumns, parsed, columnKeys); };

    internal::RunStaticParserPipeline<CsvByteRange, CsvWorkerState>(source, sink, options, advanced, stageA, stageB);
}

} // namespace loglib
