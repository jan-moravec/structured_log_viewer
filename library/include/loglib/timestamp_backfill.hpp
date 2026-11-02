#pragma once

#include "loglib/log_configuration.hpp"
#include "loglib/log_data.hpp"
#include "loglib/log_line.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace loglib
{

/** @brief Promotes timestamp columns in @p logData; returns per-line failure messages. */
std::vector<std::string> ParseTimestamps(LogData &logData, const LogConfiguration &configuration);

/**
 * @brief Promotes one configured `Type::Time` column over @p lines in place.
 * Caller must ensure `column.type == Type::Time`. Pass a sub-span to
 * restrict the back-fill to a slice of a larger vector (e.g. only the rows
 * just appended in a streaming batch). Returns per-line failure messages.
 */
std::vector<std::string> BackfillTimestampColumn(const Column &column, std::span<LogLine> lines);

/**
 * @brief Tag selecting the `void` overload that skips per-line "Failed to parse"
 * formatting on the streaming hot path.
 */
enum class BackfillErrors : uint8_t
{
    Discard
};

/** @brief `void` overload of `BackfillTimestampColumn` that drops error messages. */
void BackfillTimestampColumn(const Column &column, std::span<LogLine> lines, BackfillErrors discardErrors);

} // namespace loglib
