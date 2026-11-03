/**
 * @brief Splits one CSV line into cells.
 *
 * The tokenizer handles quoted cells and doubled quotes. It hands each cell to a callback and uses a scratch string for
 * cells that need unescaping, so plain cells stay views into the line.
 *
 * This is part of the parse module.
 */
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace loglib::internal
{

/**
 * @brief One cell of a CSV record.
 *
 * A quoted cell stays a string. When fromScratch is set, the value points into the caller's unescape buffer and must be
 * copied before that buffer is reused.
 */
struct CsvCell
{
    std::string_view value;
    bool wasQuoted = false;
    bool fromScratch = false;
};

/**
 * @brief Splits one CSV record into cells, following RFC 4180 quoting but recovering from small errors.
 *
 * emit is called once for each cell. The function returns false for a quoted cell that is not closed, which includes a
 * cell that runs over several lines, because those are not supported.
 *
 * - Cells are separated by commas.
 * - A cell that starts with a quote ends at the next quote that is not doubled, and a doubled quote stands for one
 *   quote.
 * - An unquoted cell ends at the next comma or at the end of the line.
 * - A comma at the end of the line gives one more, empty cell.
 *
 * Unescaped quoted cells are built in quotedScratch.
 */
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
                // This also covers cells that continue on the next line, which are not supported.
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
                // "a"x ends with the quoted cell, not an empty one.
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
