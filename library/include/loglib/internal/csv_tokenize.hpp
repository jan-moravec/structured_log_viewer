#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace loglib::internal
{

/// One cell from a CSV record.
///
/// `wasQuoted` disables typed-value detection at the parser layer
/// (mirroring logfmt: a quoted value is always a literal string).
/// `fromScratch` means `value` points into the caller-supplied
/// scratch buffer (`""`-unescaped) and must be copied before the
/// next cell overwrites it.
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
///
/// Kept separate from `csv_parser.cpp` so the static and streaming
/// CSV paths share the exact same tokenisation.
template <class Emit>
bool TokenizeCsvLine(std::string_view line, std::string &quotedScratch, Emit emit)
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

            // After a quoted cell, only `,` or EOL is conformant;
            // tolerate stray bytes by skipping to the next `,`
            // (matches logfmt's lax stance).
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
