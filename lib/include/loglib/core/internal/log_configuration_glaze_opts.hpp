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
 * Shared glaze options for LogConfiguration read/write.
 *
 * Pinned behaviours:
 * 1. prettify = true + 4-space indent so session JSON is
 *    human-inspectable (the recents store is a debugging surface).
 * 2. error_on_unknown_keys = false so unknown fields, such as those
 *    written by a newer build, load instead of throwing. This backs
 *    the promise that a failed source rebind is non-fatal.
 */
struct LogConfigOpts : glz::opts
{
    uint8_t indentation_width = 4; // NOLINT(readability-identifier-naming)
};
// MSVC enforces member-declaration order in designated init.
constexpr LogConfigOpts LOG_CONFIG_OPTS{{.error_on_unknown_keys = false, .prettify = true}};

} // namespace loglib::internal
