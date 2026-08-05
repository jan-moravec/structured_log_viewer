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

/// Out-parameter overload. Appends the serialized row to @p out; the
/// caller is responsible for clearing @p out between rows when it
/// wants each row's bytes in isolation. Reserved so hot serialisation
/// loops (bundle export, row export) can retain the buffer's
/// capacity across rows instead of paying a fresh `std::string`
/// allocation per record.
void SerializeNormalizedJsonRow(const LogLine &line, const KeyIndex &keys, std::string &out);

} // namespace loglib::internal
