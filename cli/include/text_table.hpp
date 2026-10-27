#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace slp
{

/**
 * @brief Terminal columns @p text occupies.
 *
 * Counts each UTF-8 code point by its East Asian width, so wide
 * characters count two and combining marks count zero. An invalid byte
 * counts one.
 */
[[nodiscard]] std::size_t DisplayWidth(std::string_view text);

/**
 * @brief Left-aligned text table for human-readable command output.
 *
 * Columns are separated by two spaces and padded to their widest cell
 * by `DisplayWidth`; the last column is not padded, so no line ends in
 * spaces. A row may carry an ANSI style for its first cell, which does
 * not count toward the width.
 */
class TextTable
{
public:
    /**
     * @param headers Header row, or empty for a table without one.
     * @param indent Text written before every line.
     */
    explicit TextTable(std::vector<std::string> headers = {}, std::string indent = {});

    /**
     * @brief Appends a row.
     *
     * @param cells Cell texts, at most as many as the widest row.
     * @param firstCellStyle ANSI sequence that starts the first cell's style, or empty.
     */
    void AddRow(std::vector<std::string> cells, std::string_view firstCellStyle = {});

    /** @brief Replaces the text of one body cell. */
    void SetCell(std::size_t row, std::size_t column, std::string text);

    /** @brief Number of body rows. */
    [[nodiscard]] std::size_t RowCount() const noexcept
    {
        return mRows.size();
    }

    /** @brief Display width of @p column across the header and every row. */
    [[nodiscard]] std::size_t ColumnWidth(std::size_t column) const;

    /** @brief Width of every column before @p column, including separators. */
    [[nodiscard]] std::size_t WidthBefore(std::size_t column) const;

    /** @brief The header row, if any, then every body row, each ending in a newline. */
    [[nodiscard]] std::string Render() const;

private:
    struct Row
    {
        std::vector<std::string> cells;
        std::string firstCellStyle;
    };

    [[nodiscard]] std::size_t ColumnCount() const noexcept;
    void RenderRow(std::string &out, const Row &row, const std::vector<std::size_t> &widths) const;

    std::vector<std::string> mHeaders;
    std::string mIndent;
    std::vector<Row> mRows;
};

} // namespace slp
