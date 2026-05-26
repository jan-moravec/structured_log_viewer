#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

namespace loglib
{

/// Microsecond-precision timestamp.
using TimeStamp = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;

/// One field's value. `string_view` aliases the memory-mapped log file
/// (which outlives every referencing `LogLine`); `string` covers values
/// that cannot live in the mmap (e.g. JSON-escape-decoded strings).
///
/// Adding new alternatives is safest at the end; consumers should switch
/// on the alternative type, not the index.
using LogValue =
    std::variant<std::string_view, std::string, int64_t, uint64_t, double, bool, TimeStamp, std::monostate>;

/// Cold-path key->value map for tests and debug dumps.
using LogMap = std::unordered_map<std::string, LogValue>;

/// Tag opting a `SetValue` caller into `string_view` storage. Use only when
/// the view's bytes outlive the `LogLine` (e.g. point into the mmap).
struct LogValueTrustView
{
};

std::optional<std::string_view> AsStringView(const LogValue &value);
bool HoldsString(const LogValue &value);
LogValue ToOwnedLogValue(const LogValue &value);

/// Treats `string_view` and `string` alternatives as equal when bytes match.
bool LogValueEquivalent(const LogValue &lhs, const LogValue &rhs);

/// Epoch microseconds for time-shaped slots (`TimeStamp`, `int64_t`, or
/// `uint64_t` <= `int64_t::max`); `nullopt` otherwise. Matches the slot
/// acceptance set of `TimeRangeRowPredicate`.
[[nodiscard]] std::optional<int64_t> AsEpochMicroseconds(const LogValue &value);

} // namespace loglib
