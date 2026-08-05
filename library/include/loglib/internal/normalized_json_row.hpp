#pragma once

#include <string>

namespace loglib
{
class KeyIndex;
class LogLine;
}

namespace loglib::internal
{

/// Serialize every present field in @p line as one compact typed JSON
/// object. Field order follows KeyId order. The returned string does not
/// include a trailing newline.
[[nodiscard]] std::string SerializeNormalizedJsonRow(const LogLine &line, const KeyIndex &keys);

} // namespace loglib::internal
