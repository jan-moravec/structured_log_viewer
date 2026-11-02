#pragma once

#include <string_view>

namespace loglib
{

/**
 * @brief Project version of this `loglib` build.
 *
 * The string is the CMake `PROJECT_VERSION` baked in at compile
 * time. `slp --version` prints it beside the CLI version.
 */
[[nodiscard]] std::string_view Version() noexcept;

} // namespace loglib
