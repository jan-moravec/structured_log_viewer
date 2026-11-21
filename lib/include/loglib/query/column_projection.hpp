/**
 * @brief Which columns are visible, in display order.
 *
 * A ColumnProjection reads the visible flag of each configured column and lists the visible column indices. Hidden
 * columns stay in the layout but are left out. Exporters and the table view use it to decide what to show or
 * write.
 *
 * This is part of the query module.
 */
#pragma once

#include "loglib/config/column_layout.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace loglib
{

/**
 * @brief The indices of the visible columns of a layout, in display order.
 *
 * Only Column::visible matters. Hidden columns stay in the layout and are left out of the result. If all columns
 * are hidden, the list is empty.
 *
 * A row that lacks a key is not handled here. CSV and Markdown output still write the column, with an empty cell. JSON
 * Lines output leaves out keys that the row does not have, and with includeAllFieldsForJson it writes every field the
 * row has.
 */
class ColumnProjection
{
public:
    /** @brief Selects the visible columns from a list of columns. */
    explicit ColumnProjection(std::span<const Column> columns);

    /** @brief Selects the visible columns of a layout. */
    explicit ColumnProjection(const ColumnLayout &layout);

    /** @brief Returns visible column indices in display order. */
    [[nodiscard]] const std::vector<std::size_t> &Indices() const noexcept
    {
        return mIndices;
    }

    /** @brief Returns whether no column is visible. */
    [[nodiscard]] bool Empty() const noexcept
    {
        return mIndices.empty();
    }

    /** @brief Returns the number of visible columns. */
    [[nodiscard]] std::size_t Size() const noexcept
    {
        return mIndices.size();
    }

private:
    std::vector<std::size_t> mIndices;
};

} // namespace loglib
