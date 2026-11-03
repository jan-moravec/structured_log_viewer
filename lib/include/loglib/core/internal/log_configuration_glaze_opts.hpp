/**
 * @brief Shared JSON options for reading and writing configuration files.
 *
 * Configuration and theme files are written with indentation so people can read them, and unknown keys are ignored so
 * files written by newer versions still load.
 *
 * This is part of the core module.
 */
#pragma once

#include <glaze/glaze.hpp>

#include <cstdint>

namespace loglib::internal
{

/**
 * @brief The JSON options used to read and write a LogConfiguration.
 *
 * Output is indented with 4 spaces, so a saved configuration is easy to inspect. Unknown fields are ignored on reading,
 * so a file written by a newer build still loads. That is also what lets a failed source rebind be non-fatal.
 */
struct LogConfigOpts : glz::opts
{
    uint8_t indentation_width = 4; // NOLINT(readability-identifier-naming)
};
// MSVC requires designated initializers to follow the declaration order of the members.
constexpr LogConfigOpts LOG_CONFIG_OPTS{{.error_on_unknown_keys = false, .prettify = true}};

} // namespace loglib::internal
