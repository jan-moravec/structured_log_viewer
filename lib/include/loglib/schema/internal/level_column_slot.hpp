/**
 * @brief The rule that decides whether a Level column moves to the canonical level slot.
 *
 * A column that becomes a Level column moves to CANONICAL_LEVEL_COLUMN_INDEX so rows read as time, severity, message.
 * ShouldBubbleLevelColumn is that rule on its own. LogTable applies it when it resolves a placement, and the tests of
 * the column configuration use it to check the rotation.
 *
 * This is part of the schema module.
 */
#pragma once

#include "loglib/schema/log_configuration.hpp"

#include <cstddef>

namespace loglib
{

/**
 * @brief True if the column should move to CANONICAL_LEVEL_COLUMN_INDEX. False if the index is out of range, the
 * column is already there, there is only one column, or another Level column holds that place.
 */
[[nodiscard]] bool ShouldBubbleLevelColumn(const LogConfiguration &config, size_t columnIndex) noexcept;

} // namespace loglib
