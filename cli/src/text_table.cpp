#include "text_table.hpp"

#include "ansi.hpp"

#include <utf8proc.h>

#include <algorithm>
#include <utility>

namespace slp
{

namespace
{

constexpr std::string_view COLUMN_SEPARATOR = "  ";

} // namespace

std::size_t DisplayWidth(std::string_view text)
{
    std::size_t width = 0;
    std::size_t index = 0;
    while (index < text.size())
    {
        utf8proc_int32_t codePoint = 0;
        const utf8proc_ssize_t size = utf8proc_iterate(
            reinterpret_cast<const utf8proc_uint8_t *>(text.data() + index),
            static_cast<utf8proc_ssize_t>(text.size() - index),
            &codePoint
        );
        if (size <= 0 || codePoint < 0)
        {
            ++width;
            ++index;
            continue;
        }
        width += static_cast<std::size_t>(std::max(0, utf8proc_charwidth(codePoint)));
        index += static_cast<std::size_t>(size);
    }
    return width;
}

TextTable::TextTable(std::vector<std::string> headers, std::string indent)
    : mHeaders(std::move(headers)), mIndent(std::move(indent))
{
}

void TextTable::AddRow(std::vector<std::string> cells, std::string_view firstCellStyle)
{
    mRows.push_back(Row{.cells = std::move(cells), .firstCellStyle = std::string(firstCellStyle)});
}

void TextTable::SetCell(std::size_t row, std::size_t column, std::string text)
{
    auto &cells = mRows.at(row).cells;
    if (cells.size() <= column)
    {
        cells.resize(column + 1);
    }
    cells[column] = std::move(text);
}

std::size_t TextTable::ColumnCount() const noexcept
{
    std::size_t count = mHeaders.size();
    for (const Row &row : mRows)
    {
        count = std::max(count, row.cells.size());
    }
    return count;
}

std::size_t TextTable::ColumnWidth(std::size_t column) const
{
    std::size_t width = column < mHeaders.size() ? DisplayWidth(mHeaders[column]) : 0;
    for (const Row &row : mRows)
    {
        if (column < row.cells.size())
        {
            width = std::max(width, DisplayWidth(row.cells[column]));
        }
    }
    return width;
}

std::size_t TextTable::WidthBefore(std::size_t column) const
{
    std::size_t width = DisplayWidth(mIndent);
    for (std::size_t index = 0; index < column; ++index)
    {
        width += ColumnWidth(index) + COLUMN_SEPARATOR.size();
    }
    return width;
}

std::string TextTable::Render() const
{
    std::vector<std::size_t> widths(ColumnCount());
    for (std::size_t column = 0; column < widths.size(); ++column)
    {
        widths[column] = ColumnWidth(column);
    }

    std::string out;
    if (!mHeaders.empty())
    {
        RenderRow(out, Row{.cells = mHeaders, .firstCellStyle = {}}, widths);
    }
    for (const Row &row : mRows)
    {
        RenderRow(out, row, widths);
    }
    return out;
}

void TextTable::RenderRow(std::string &out, const Row &row, const std::vector<std::size_t> &widths) const
{
    std::string line = mIndent;
    // Trailing empty cells are dropped with their separators, so no line ends in padding.
    std::size_t shown = row.cells.size();
    while (shown > 1 && row.cells[shown - 1].empty())
    {
        --shown;
    }
    for (std::size_t column = 0; column < shown; ++column)
    {
        const std::string &cell = row.cells[column];
        if (column != 0)
        {
            line.append(COLUMN_SEPARATOR);
        }
        if (column == 0 && !row.firstCellStyle.empty())
        {
            line.append(row.firstCellStyle).append(cell).append(ANSI_RESET);
        }
        else
        {
            line.append(cell);
        }
        if (column + 1 < shown)
        {
            line.append(widths[column] - DisplayWidth(cell), ' ');
        }
    }
    out.append(line).push_back('\n');
}

} // namespace slp
