/**
 * @brief Variant type for a field value and helpers to read it.
 *
 * LogValue is what a caller sees when it asks a row for a field: text, a number, a boolean, a timestamp or nothing.
 * Rows themselves store the compact form. The helpers here read a value as text or as epoch microseconds and compare
 * values regardless of how their text is held.
 *
 * This is part of the core module.
 */
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

/** @brief Microsecond-precision timestamp. */
using TimeStamp = std::chrono::time_point<std::chrono::system_clock, std::chrono::microseconds>;

/**
 * @brief The value of one field.
 *
 * A string_view points into the memory-mapped file, which outlives every row that uses it. A string holds a value that
 * cannot stay in the file, such as a decoded JSON escape. Choose the alternative by its type and not by its index.
 */
using LogValue =
    std::variant<std::string_view, std::string, int64_t, uint64_t, double, bool, TimeStamp, std::monostate>;

/** @brief A row as a map from key name to value. Slow, so use it for tests and debug output only. */
using LogMap = std::unordered_map<std::string, LogValue>;

/**
 * @brief Passed to LogLine::SetValue to promise that a string_view value outlives the row, for example by pointing
 * into the mapped file.
 */
struct LogValueTrustView
{
};

std::optional<std::string_view> AsStringView(const LogValue &value);
bool HoldsString(const LogValue &value);
LogValue ToOwnedLogValue(const LogValue &value);

/** @brief Treats string_view and string alternatives as equal when bytes match. */
bool LogValueEquivalent(const LogValue &lhs, const LogValue &rhs);

/**
 * @brief Returns the value as microseconds since the epoch if it can be a time, otherwise nullopt.
 *
 * A TimeStamp, an int64_t, and a uint64_t that fits in int64_t count as times. Time filters accept exactly these.
 */
[[nodiscard]] std::optional<int64_t> AsEpochMicroseconds(const LogValue &value);

} // namespace loglib
