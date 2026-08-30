#pragma once

#include "loglib/column_schema.hpp"
#include "loglib/compact_log_value.hpp"
#include "loglib/key_index.hpp"
#include "loglib/log_line.hpp"

#include <span>

namespace loglib
{

/**
 * @brief "Does this column's data match its configured `Type`?"
 *
 * Computed on demand for the diagnostics UI; one column-walk per
 * call, no hot-path bookkeeping. Stateless: borrows row storage
 * and the column descriptor for the duration of
 * `ComputeColumnTypeHealth`.
 */
struct ColumnTypeHealth
{
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    /** @brief Total rows in the table. */
    size_t totalSlots = 0;
    /** @brief Rows where this column carries any (non-monostate) value. */
    size_t presentSlots = 0;
    /**
     * @brief Present slots whose variant matches the configured `Type`.
     * `Type::Any` matches every present slot. For
     * `Enumeration` / `Level`, `DictRef` slots match; unencoded
     * raw-string slots count as present-but-not-matching (this
     * is how user-pinned dict columns expose over-cap values).
     */
    size_t matchingSlots = 0;
    // NOLINTEND(misc-non-private-member-variables-in-classes)

    [[nodiscard]] constexpr bool operator==(const ColumnTypeHealth &) const = default;
};

/**
 * @brief Returns whether @p tag is a valid representation of @p declaredType.
 *
 * Mirrors the variants accepted by the sort / filter comparators in
 * `log_compare.cpp` / `log_filter.cpp`. `Monostate` never matches.
 */
[[nodiscard]] bool TagMatchesType(CompactTag tag, ColumnType declaredType) noexcept;

/**
 * @brief Walks @p lines once and counts present vs type-matching slots for
 * @p column. Out-of-range or keyless columns yield zeros except
 * `totalSlots` (the line count) when @p column has no interned keys.
 *
 * @param lines Table rows to inspect; not retained.
 * @param keys Intern table used to resolve `column.keys`.
 * @param column Schema entry whose `type` and `keys` drive the walk.
 * @return Slot counts for diagnostics UI.
 */
[[nodiscard]] ColumnTypeHealth ComputeColumnTypeHealth(
    std::span<const LogLine> lines, const KeyIndex &keys, const Column &column
);

} // namespace loglib
