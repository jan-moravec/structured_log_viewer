#pragma once

#include "loglib/log_configuration.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace loglib
{

/**
 * @brief Visible column indices of a `LogConfiguration`, in display order.
 *
 * Reads `Column::visible` only. Hidden columns stay in the
 * configuration; they are omitted from the result. An all-hidden
 * configuration yields an empty index list.
 *
 * Missing keys on a row are not this type's concern. CSV and
 * Markdown still emit a projected column (empty / monostate cell).
 * JSON Lines omits keys that are absent from the row. When
 * `includeAllFieldsForJson` is true it emits every field present
 * on the row.
 */
class ColumnProjection
{
public:
    /**
     * @brief Projects @p columns to visible indices.
     *
     * @param columns Configuration columns in display order.
     */
    explicit ColumnProjection(std::span<const LogConfiguration::Column> columns);

    /**
     * @brief Projects @p configuration.columns to visible indices.
     *
     * @param configuration Configuration whose columns to project.
     */
    explicit ColumnProjection(const LogConfiguration &configuration);

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
