#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace loglib::internal
{

/// One cell from a CSV record.
///
/// Quoted cells remain strings. `fromScratch` means `value` points
/// into the caller's unescape buffer and must be copied before reuse.
struct CsvCell
{
    std::string_view value;
    bool wasQuoted = false;
    bool fromScratch = false;
};

/// Tokenize one CSV record using RFC 4180 quoting with lax recovery.
/// Calls @p emit per cell and rejects unterminated quoted cells.
///
/// Grammar: cells separated by `,`; a leading `"` opens a quoted cell
/// closed by an unescaped `"`, with `""` decoded to a literal `"`; an
/// unquoted cell ends at the next `,` or EOL; a trailing `,` emits one
/// final empty cell.
///
/// @p quotedScratch stores unescaped quoted cells.
template <class Emit> bool TokenizeCsvLine(std::string_view line, std::string &quotedScratch, Emit emit)
{
    const char *const data = line.data();
    const std::size_t end = line.size();
    std::size_t i = 0;

    while (true)
    {
        if (i < end && data[i] == '"')
        {
            ++i;
            const std::size_t innerStart = i;
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
                for (std::size_t j = 0; j < rawInner.size(); ++j)
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

            // Tolerate bytes after a closing quote by skipping to
            // the next comma.
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
                // `"a"x` ends with the quoted cell, not an empty one.
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

        const std::size_t cellStart = i;
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

} // namespace loglib::internal
