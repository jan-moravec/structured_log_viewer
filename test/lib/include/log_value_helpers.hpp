/**
 * @brief Helpers for comparing and copying LogValue in tests.
 *
 * The library reads values through AsStringView and AsEpochMicroseconds. These helpers are only for assertions: they
 * tell whether a value holds text, copy a value so that it no longer points into a source, and compare two values while
 * treating text held as a string and as a string_view as equal.
 *
 * This is a test helper. It is not part of the library.
 */
#pragma once

#include "loglib/core/log_value.hpp"

#include <string>
#include <string_view>
#include <variant>

namespace loglib
{

/** @brief True if the value holds text, as a string or as a string_view. */
[[nodiscard]] inline bool HoldsString(const LogValue &value)
{
    return std::holds_alternative<std::string_view>(value) || std::holds_alternative<std::string>(value);
}

/** @brief Copies a string_view into a string and returns every other alternative unchanged. */
[[nodiscard]] inline LogValue ToOwnedLogValue(const LogValue &value)
{
    if (const auto *view = std::get_if<std::string_view>(&value))
    {
        return std::string(*view);
    }
    return value;
}

/** @brief Treats string_view and string alternatives as equal when their bytes match. */
[[nodiscard]] inline bool LogValueEquivalent(const LogValue &lhs, const LogValue &rhs)
{
    const auto lhsString = AsStringView(lhs);
    const auto rhsString = AsStringView(rhs);
    if (lhsString.has_value() || rhsString.has_value())
    {
        return lhsString.has_value() && rhsString.has_value() && *lhsString == *rhsString;
    }
    return lhs == rhs;
}

} // namespace loglib
