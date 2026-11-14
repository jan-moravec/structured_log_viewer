/**
 * @brief Glaze read and write options shared by every JSON file the library reads or writes.
 *
 * Configuration, regex template, theme and session bundle files are written with indentation so people can read them.
 * Regex template and theme files ignore unknown keys when reading, and configuration files reject them. The schema,
 * parse, support and output modules all use these options, which is why the header is in core.
 *
 * This is part of the core module.
 */
#pragma once

#include <glaze/glaze.hpp>

#include <cstdint>

namespace loglib::internal
{

/**
 * @brief The JSON options used for the library's JSON files.
 *
 * Output is indented with 4 spaces. Unknown fields are ignored on reading, which also lets a failed source rebind be
 * non-fatal.
 */
struct LogConfigOpts : glz::opts
{
    uint8_t indentation_width = 4; // NOLINT(readability-identifier-naming)
};
// MSVC requires designated initializers to follow the declaration order of the members.
// null_terminated is off because the readers take a string_view, which has no terminator, and Glaze would read one byte
// past its end.
constexpr LogConfigOpts LOG_CONFIG_OPTS{{.null_terminated = false, .error_on_unknown_keys = false, .prettify = true}};

/**
 * @brief The JSON options for configuration files and the configuration inside a session bundle.
 *
 * Unknown keys are errors. A key is required only where a type says so, through its requires_key function, so a missing
 * optional key keeps the default of its member. Output is indented.
 */
constexpr LogConfigOpts CONFIGURATION_OPTS{
    {.null_terminated = false, .error_on_unknown_keys = true, .prettify = true, .error_on_missing_keys = true}
};

/** @brief The same rules as CONFIGURATION_OPTS, with the output on one line. */
constexpr LogConfigOpts CONFIGURATION_COMPACT_OPTS{
    {.null_terminated = false, .error_on_unknown_keys = true, .prettify = false, .error_on_missing_keys = true}
};

} // namespace loglib::internal
