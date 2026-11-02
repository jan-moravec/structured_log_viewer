/**
 * @brief Version string of this build of the library.
 *
 * The value is the project version baked in at compile time. The command line tool prints it next to its own version.
 *
 * This is part of the core module.
 */
#pragma once

#include <string_view>

namespace loglib
{

/**
 * @brief Project version of this loglib build.
 *
 * The string is the CMake PROJECT_VERSION baked in at compile
 * time. slp --version prints it beside the CLI version.
 */
[[nodiscard]] std::string_view Version() noexcept;

} // namespace loglib
