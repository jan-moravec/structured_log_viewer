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
 * Serialize @p line as a compact typed JSON object in KeyId order.
 * The result has no trailing newline.
 */
[[nodiscard]] std::string SerializeNormalizedJsonRow(const LogLine &line, const KeyIndex &keys);

/**
 * Append the same representation to @p out, allowing callers to
 * reuse its capacity across rows.
 */
void SerializeNormalizedJsonRow(const LogLine &line, const KeyIndex &keys, std::string &out);

} // namespace loglib::internal
