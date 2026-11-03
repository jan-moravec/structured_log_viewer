/**
 * @brief Snapshot of all regex templates for format detection.
 *
 * Detection tries built-in templates before user templates. This header returns that merged list and a counter that
 * changes when user templates change, so cached compiled patterns can be refreshed.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/parse/regex_templates.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace loglib::internal
{

/**
 * @brief Returns the built-in templates followed by the registered extra templates, each group sorted by priority.
 *
 * It is the order the template list uses, and the detection in the regex parser needs it to see the user's templates
 * without reaching into regex_templates.cpp. The result is shared, so a parser can keep it for as long as it refers to
 * the templates. Its pointer changes, together with TemplatesGeneration, whenever SetExtraRegexTemplates replaces the
 * extras.
 */
[[nodiscard]] std::shared_ptr<const std::vector<RegexTemplate>> MergedRegexTemplates();

/**
 * @brief A counter that goes up each time the extra templates are replaced, including the first build.
 *
 * It is cheap enough for the detection hot path. A parser remembers the last value it saw and asks for
 * MergedRegexTemplates again only when the counter has moved, so the usual cost is one atomic load.
 */
[[nodiscard]] uint64_t TemplatesGeneration() noexcept;

} // namespace loglib::internal
