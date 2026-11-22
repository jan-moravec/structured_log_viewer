/**
 * @brief The matching rule that regex detection uses on a sample.
 *
 * Detection looks at the first non-blank lines of a sample and asks whether a pattern matches enough of them. The
 * catalog uses this rule to pick a template, and SampleFitsFormat uses it to check a pattern the caller chose, so the
 * two always agree. Only library sources include this header, because it pulls in the PCRE2 C API.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/parse/internal/regex_compiled_pattern.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace loglib::internal
{

/**
 * @brief How many probe lines a pattern must match to claim a sample. Two rule out a single line that happens to look
 * like syslog, without turning down short files.
 */
inline constexpr std::size_t PROBE_MIN_MATCHES = 2;

/** @brief The most non-blank lines collected for detection, so a huge sample costs no more than a small one. */
inline constexpr std::size_t PROBE_MAX_LINES = 8;

/**
 * @brief Collects the first non-blank lines of a sample, without the byte order mark.
 *
 * It stops after PROBE_MAX_LINES lines or PROBE_BYTES_BUDGET bytes.
 */
[[nodiscard]] std::vector<std::string> CollectProbeLines(std::string_view sample);

/**
 * @brief True if the pattern matches at least PROBE_MIN_MATCHES of the lines, each matched as a whole line.
 *
 * Anchoring at both ends keeps a pattern without anchors from claiming a line where it matches only a part. Each call
 * allocates its own match data, so one compiled pattern can be used from many threads.
 */
[[nodiscard]] bool PatternFitsLines(const CompiledPattern &pattern, std::span<const std::string> lines);

} // namespace loglib::internal
