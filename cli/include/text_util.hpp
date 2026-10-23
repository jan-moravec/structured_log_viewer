#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace slp
{

/** @brief Returns @p text with ASCII letters lower-cased. */
[[nodiscard]] std::string AsciiLower(std::string_view text);

/** @brief Whether @p lhs and @p rhs are equal, ignoring ASCII case. */
[[nodiscard]] bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept;

/** @brief Whether @p text starts with @p prefix, ignoring ASCII case. */
[[nodiscard]] bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) noexcept;

/** @brief Joins @p parts with `", "`. */
[[nodiscard]] std::string JoinComma(const std::vector<std::string> &parts);

/** @brief Splits @p text on commas, trims surrounding whitespace, and drops empty parts. */
[[nodiscard]] std::vector<std::string> SplitCsv(std::string_view text);

} // namespace slp
