#pragma once

#include <string>

namespace loglib
{
class KeyIndex;
class LogLine;
}

namespace loglib::internal
{

/// Serialize @p line as a compact typed JSON object in KeyId order.
/// The result has no trailing newline.
[[nodiscard]] std::string SerializeNormalizedJsonRow(const LogLine &line, const KeyIndex &keys);

/// Append the same representation to @p out, allowing callers to
/// reuse its capacity across rows.
void SerializeNormalizedJsonRow(const LogLine &line, const KeyIndex &keys, std::string &out);

} // namespace loglib::internal
