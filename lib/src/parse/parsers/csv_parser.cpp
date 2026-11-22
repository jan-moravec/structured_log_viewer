#include "loglib/parse/parsers/csv_parser.hpp"

#include "loglib/parse/format_detection.hpp"

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
#include <memory>
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

/** Removes a UTF-8 byte order mark from the start of the text. */
std::string_view StripBom(std::string_view sv) noexcept
{
    if (sv.starts_with(UTF8_BOM))
    {
        sv.remove_prefix(UTF8_BOM.size());
    }
    return sv;
}

/** Removes a carriage return from the end of the line, which handles CRLF line endings. */
std::string_view StripCr(std::string_view sv) noexcept
{
    if (!sv.empty() && sv.back() == '\r')
    {
        sv.remove_suffix(1);
    }
    return sv;
}

/**
 * Renames duplicate column names. The first keeps its name, and the later ones get the suffix _2, _3 and so on,
 * skipping names another column already has. A row needs each key once.
 */
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

/**
 * Reads a header line into the key id of each column, in the order of the cells of the data rows. internKey turns a
 * column name into its key id, and duplicate names are renamed first. Returns false when the header is empty or a
 * quoted cell is not closed.
 */
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

/** Counts the cells of a line, or returns nothing when a quoted cell is not closed. Only IsValid uses it. */
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

/**
 * Reads one record into compact values, with the key ids taken from the header, so no key lookup runs per cell. An
 * empty cell gives no field, which reads the same as an empty value. outRaggedExtra reports cells past the last header
 * column, which the caller turns into an error.
 */
void ParseCsvLine(
    std::string_view line,
    const std::vector<KeyId> &columnKeys,
    const char *fileBegin,
    size_t fileSize,
    std::string &ownedArena,
    std::string &quotedScratch,
    std::vector<loglib::CompactLogValue> &out,
    bool &outUnterminated,
    size_t &outRaggedExtra
)
{
    out.clear();
    // Exact upper bound: extras land in outRaggedExtra, not out.
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
        out.push_back(compact.WithKey(keyId));
    });

    outUnterminated = !ok;
}

/**
 * Keeps IsValid from accepting other formats: a CSV header has at least two cells, and at least two of them are not
 * empty.
 */
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

/**
 * The CSV decoder. A stream decoder reads the header from the first line that is not blank and returns Skip for it. A
 * file decoder is given the key ids of the header, which the parser read up front, and the position of the header in
 * the file, where it returns Skip. The later lines use the key ids of the header.
 */
class CsvLineDecoder
{
public:
    /** A UTF-8 byte order mark can start the header line. */
    static constexpr bool kStripBom = true;

    /** A decoder that reads the header from the first line it gets. */
    CsvLineDecoder() = default;

    /** A decoder for a file whose header was read already. headerOffset is where the header text starts in the file, if
     * the file has one. */
    CsvLineDecoder(std::shared_ptr<const std::vector<KeyId>> header, std::optional<uint64_t> headerOffset)
        : mHeaderConsumed(true), mSharedColumnKeys(std::move(header)), mHeaderOffset(headerOffset)
    {
    }

    internal::LineDecodeResult Decode(
        std::string_view line,
        internal::LineContext &ctx,
        std::vector<loglib::CompactLogValue> &out,
        std::string &errorOut
    )
    {
        out.clear();

        if (!mHeaderConsumed)
        {
            const auto internKey = [&](std::string_view name) {
                return internal::InternKeyVia(name, ctx.keys, ctx.keyCache);
            };
            if (!ParseHeaderLine(line, internKey, mQuotedScratch, mOwnColumnKeys))
            {
                errorOut = "Invalid CSV header.";
                return internal::LineDecodeResult::Error;
            }
            mHeaderConsumed = true;
            return internal::LineDecodeResult::Skip;
        }
        if (mHeaderOffset.has_value() && !ctx.stableBytes.empty() &&
            std::cmp_equal(line.data() - ctx.stableBytes.data(), *mHeaderOffset))
        {
            return internal::LineDecodeResult::Skip;
        }

        const std::vector<KeyId> &columnKeys = mSharedColumnKeys ? *mSharedColumnKeys : mOwnColumnKeys;
        try
        {
            bool unterminated = false;
            size_t raggedExtra = 0;
            ParseCsvLine(
                line,
                columnKeys,
                ctx.stableBytes.data(),
                ctx.stableBytes.size(),
                ctx.ownedBytes,
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
                    "Row has {} extra cell(s) beyond the {}-column header.", raggedExtra, columnKeys.size()
                );
                return internal::LineDecodeResult::Error;
            }
            // A row stores its fields by ascending key id, and the header order may differ.
            std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) { return a.Key() < b.Key(); });
            return internal::LineDecodeResult::Emit;
        }
        catch (const std::exception &e)
        {
            errorOut = std::string(e.what());
            return internal::LineDecodeResult::Error;
        }
    }

    [[nodiscard]] KeyId ContinuationTarget() const noexcept
    {
        return INVALID_KEY_ID;
    }

private:
    bool mHeaderConsumed = false;
    std::shared_ptr<const std::vector<KeyId>> mSharedColumnKeys;
    std::optional<uint64_t> mHeaderOffset;
    std::vector<KeyId> mOwnColumnKeys;
    std::string mQuotedScratch;
};

static_assert(internal::LineDecoder<CsvLineDecoder>, "CsvLineDecoder must satisfy the LineDecoder concept");

} // namespace

bool LooksLikeCsv(std::string_view sniffBuffer)
{
    // Callers cap sniffBuffer at PROBE_BYTES_BUDGET, so the
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

void CsvParser::Parse(StreamLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    CsvLineDecoder decoder;
    internal::RunStreamingParseLoop(source, decoder, sink, options);
}

void CsvParser::Parse(FileLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    const std::span<const char> bytes = source.StableBytes();

    const char *fileBegin = bytes.data();
    const size_t fileSize = bytes.size();
    const char *fileEnd = (fileBegin != nullptr) ? fileBegin + fileSize : nullptr;

    // Eagerly find and parse the header. Outputs:
    //  * columnKeys -- shared column -> KeyId table for Stage B.
    //  * headerLineOffset -- exact byte offset Stage B uses to skip
    //    the header (handles the case where leading blanks push the
    //    header past batch 0).
    //  * headerParseFailed -- short-circuits the data pipeline so a
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
                // Where the header text starts, after a byte order mark: the decoder sees the line without it.
                headerLineOffset = static_cast<uint64_t>(candidate.data() - fileBegin);
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
    // route through BatchCoalescer::Finish to keep the sink contract.
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

    // The header columns are interned up front, so the pipeline announces them as declared keys, even for a column that
    // is empty in every row.
    const auto header = std::make_shared<const std::vector<KeyId>>(std::move(columnKeys));
    internal::RunStaticParserPipeline<CsvLineDecoder>(
        source, sink, options, [&] { return CsvLineDecoder(header, headerLineOffset); }, *header
    );
}

} // namespace loglib
