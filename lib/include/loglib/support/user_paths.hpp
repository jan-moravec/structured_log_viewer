/**
 * @brief Per-user folders for application data.
 *
 * Returns the folder where the application keeps user data, such as regex templates, using the platform's standard
 * location. The folder names match the desktop application so both programs share one location. Nothing here creates
 * folders.
 *
 * This is part of the support module.
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{

/**
 * @brief The organization folder name used by UserDataDirectory. The desktop application uses the same name, so both
 * share one data folder.
 */
inline constexpr std::string_view USER_DATA_ORGANIZATION_NAME = "jan-moravec";

/**
 * @brief The application folder name used by UserDataDirectory. The desktop application uses the same name, so both
 * share one data folder.
 */
inline constexpr std::string_view USER_DATA_APPLICATION_NAME = "StructuredLogViewer";

/**
 * @brief Value of environment variable @p name as a path.
 *
 * Windows reads the wide-character environment, so names outside the
 * active code page survive. Call it at startup, before other threads
 * change the environment.
 *
 * @param name ASCII variable name.
 * @return The value, or std::nullopt when the variable is unset or empty.
 */
[[nodiscard]] std::optional<std::filesystem::path> EnvironmentPath(std::string_view name);

/**
 * @brief Whether environment variable @p name exists, even with an empty value.
 *
 * @param name ASCII variable name.
 */
[[nodiscard]] bool EnvironmentIsSet(std::string_view name);

/**
 * @brief The folder where the application stores its data, the same one Qt reports as the application data location.
 *
 * Windows uses %APPDATA%/<organization>/<application> and returns an
 * empty path when %APPDATA% is unset. Linux uses
 * $XDG_DATA_HOME/<organization>/<application> when that variable is
 * absolute, and ~/.local/share/<organization>/<application> otherwise.
 * A relative XDG_DATA_HOME is ignored. macOS uses
 * ~/Library/Application Support/<organization>/<application>.
 * The organization and application segments are
 * USER_DATA_ORGANIZATION_NAME and USER_DATA_APPLICATION_NAME.
 */
[[nodiscard]] std::filesystem::path UserDataDirectory();

/**
 * @brief UserDataDirectory() / "regex_templates".
 * Empty when UserDataDirectory() is empty. Does not create the directory.
 */
[[nodiscard]] std::filesystem::path UserRegexTemplateDirectory();

} // namespace loglib
