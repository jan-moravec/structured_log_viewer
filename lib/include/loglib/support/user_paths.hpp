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
 * @brief Returns the value of an environment variable as a path, or nullopt if it is not set or is empty.
 *
 * On Windows it reads the wide-character environment, so names outside the active code page are kept. Call it at
 * startup, before other threads change the environment. The name must be ASCII.
 */
[[nodiscard]] std::optional<std::filesystem::path> EnvironmentPath(std::string_view name);

/** @brief True if the environment variable exists, even with an empty value. The name must be ASCII. */
[[nodiscard]] bool EnvironmentIsSet(std::string_view name);

/**
 * @brief The folder where the application stores its data, the same one Qt reports as the application data location.
 *
 * On Windows it is %APPDATA%/organization/application, and the result is empty if %APPDATA% is not set. On Linux it is
 * $XDG_DATA_HOME/organization/application when that variable is an absolute path, and otherwise
 * ~/.local/share/organization/application. A relative XDG_DATA_HOME is ignored. On macOS it is ~/Library/Application
 * Support/organization/application. The organization and application names are USER_DATA_ORGANIZATION_NAME and
 * USER_DATA_APPLICATION_NAME.
 */
[[nodiscard]] std::filesystem::path UserDataDirectory();

/**
 * @brief The regex_templates folder inside UserDataDirectory, or an empty path if that is empty. The folder is not
 * created.
 */
[[nodiscard]] std::filesystem::path UserRegexTemplateDirectory();

} // namespace loglib
