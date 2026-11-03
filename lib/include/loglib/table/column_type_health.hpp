/**
 * @brief Checks whether a column's data matches its configured type.
 *
 * ComputeColumnTypeHealth walks one column and counts the values that do not fit the declared type. The diagnostics
 * view calls it on demand. It keeps no state and does not slow down parsing.
 *
 * This is part of the table module.
 */
#pragma once

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/key_index.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/schema/column_schema.hpp"

#include <span>

namespace loglib
{

/**
 * @brief How well the data of one column fits the type configured for it.
 *
 * It is computed on request by walking the column once, so nothing is tracked while parsing.
 */
struct ColumnTypeHealth
{
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    /** @brief Number of rows in the table. */
    size_t totalSlots = 0;
    /** @brief Number of rows where the column has a value. */
    size_t presentSlots = 0;
    /**
     * @brief Number of present values that fit the configured type.
     *
     * Type::Any accepts every present value. For Enumeration and Level columns only encoded values fit, so a value
     * that is still plain text counts as present but not matching. That is how a column the user set to an enumeration
     * shows values that were over the dictionary cap.
     */
    size_t matchingSlots = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] constexpr bool operator==(const ColumnTypeHealth &) const = default;
};

/**
 * @brief Returns whether a cell of this kind is a valid value for a column of the declared type.
 *
 * It follows what sorting and filtering accept for each type. An empty cell never matches.
 */
[[nodiscard]] bool TagMatchesType(CompactTag tag, ColumnType declaredType) noexcept;

/**
 * @brief Counts the present and the type-matching values of a column by walking the rows once.
 *
 * A column none of whose keys is interned gives zero for both counts, with totalSlots set to the number of rows.
 * The rows are not kept.
 *
 * @param keys The index used to look up the column's keys.
 */
[[nodiscard]] ColumnTypeHealth ComputeColumnTypeHealth(
    std::span<const LogLine> lines, const KeyIndex &keys, const Column &column
);

} // namespace loglib
