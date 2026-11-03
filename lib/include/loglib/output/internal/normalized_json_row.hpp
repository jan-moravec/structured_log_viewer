/**
 * @brief Serializes one row as a normalized JSON object.
 *
 * The session bundle stores each row as one compact JSON object, in key id order and with typed values. This helper
 * produces that text from a LogLine and its KeyIndex.
 *
 * This is part of the output module.
 */
#pragma once

#include <string>

namespace loglib
{
class KeyIndex;
class LogLine;
} // namespace loglib

namespace loglib::internal
{

/**
 * @brief Returns the row as a compact JSON object with typed values, ordered by key id. The text has no newline at the
 * end.
 */
[[nodiscard]] std::string SerializeNormalizedJsonRow(const LogLine &line, const KeyIndex &keys);

/** @brief Appends the same text to out, so a caller can reuse one string for many rows. */
void SerializeNormalizedJsonRow(const LogLine &line, const KeyIndex &keys, std::string &out);

} // namespace loglib::internal
