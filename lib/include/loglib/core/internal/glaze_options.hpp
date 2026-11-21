/**
 * @brief Glaze read and write options shared by every JSON file the library reads or writes.
 *
 * Configuration, regex template, theme and session bundle files are written with indentation so people can read them.
 * Every reader rejects unknown keys. A key that is missing keeps the default of its member, except in configuration
 * files, where the types say which keys are required. The config, parse, support and output modules all use these
 * options, which is why the header is in core.
 *
 * This is part of the core module.
 */
#pragma once

#include <glaze/glaze.hpp>

#include <cstdint>

namespace loglib::internal
{

/**
 * @brief The JSON options for every JSON file the library reads and writes, except configuration files.
 *
 * Regex template, theme and session bundle files use these.
 *
 * Output is indented with 4 spaces. Unknown keys are errors, and a missing key keeps the default of its member.
 */
struct JsonFileOptions : glz::opts
{
    uint8_t indentation_width = 4; // NOLINT(readability-identifier-naming)
};
// MSVC requires designated initializers to follow the declaration order of the members.
// null_terminated is off because the readers take a string_view, which has no terminator, and Glaze would read one byte
// past its end.
constexpr JsonFileOptions JSON_FILE_OPTIONS{{
    .null_terminated = false,
    .error_on_unknown_keys = true,
    .prettify = true,
}};

/**
 * @brief The JSON options for configuration files and the configuration inside a session bundle.
 *
 * Unknown keys are errors. A key is required only where a type says so, through its requires_key function, so a missing
 * optional key keeps the default of its member. Output is indented.
 */
constexpr JsonFileOptions MakeConfigurationOpts(bool prettify)
{
    JsonFileOptions opts{};
    opts.null_terminated = false;
    opts.error_on_unknown_keys = true;
    opts.error_on_missing_keys = true;
    opts.prettify = prettify;
    return opts;
}

/** @brief The options for configuration files, with indented output. */
constexpr JsonFileOptions CONFIGURATION_OPTS = MakeConfigurationOpts(true);

/** @brief The same rules as CONFIGURATION_OPTS, with the output on one line. */
constexpr JsonFileOptions CONFIGURATION_COMPACT_OPTS = MakeConfigurationOpts(false);

} // namespace loglib::internal
