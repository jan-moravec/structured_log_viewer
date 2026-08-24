#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace loglib
{

/**
 * @brief Parsed Go-to-timestamp input.
 *
 * `micros` is epoch microseconds. `isNaive` is true when the
 * accepted format has no time-zone specifier, so the caller must
 * shift through the display zone before comparing stored UTC
 * timestamps. Relative shortcuts always set `isNaive` to false.
 */
struct UserTimestampParse
{
    std::int64_t micros = 0;
    bool isNaive = false;
};

/**
 * @brief Returns whether @p format contains a zone specifier.
 *
 * Detects `%z`, `%Z`, `%Ez`, and `%Oz`. `%%` is a literal percent.
 * A malformed prefix such as `%E%z` still matches the inner `%z`.
 *
 * @param format `date::parse` format string.
 * @return `true` when a parse of this format yields UTC.
 */
[[nodiscard]] bool FormatHasZoneSpecifier(std::string_view format) noexcept;

/**
 * @brief Parses absolute or relative timestamp input.
 *
 * Tries a relative shortcut `[+-]?N[hm]` (case-insensitive,
 * whitespace-tolerant; every sign means N units before @p now),
 * then each of @p columnParseFormats, then the ISO fallbacks
 * `%FT%T` and `%F %T`. Does not apply
 * `LocalMicrosecondsSinceEpochToUtc`; the caller does that when
 * `isNaive` is true.
 *
 * @param input UTF-8 text; leading and trailing ASCII whitespace is ignored.
 * @param columnParseFormats Column `parseFormats` tried before ISO fallbacks.
 * @param now Reference instant for relative shortcuts.
 * @return Parsed timestamp, or `std::nullopt` on failure or overflow.
 */
[[nodiscard]] std::optional<UserTimestampParse> ParseUserTimestamp(
    std::string_view input,
    std::span<const std::string> columnParseFormats,
    std::chrono::system_clock::time_point now
);

} // namespace loglib
