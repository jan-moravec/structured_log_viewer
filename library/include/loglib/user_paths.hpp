#pragma once

#include "loglib/regex_templates.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace loglib
{

/** @brief Organization directory used by `UserDataDirectory`. Matches the GUI application identity. */
inline constexpr std::string_view USER_DATA_ORGANIZATION_NAME = "jan-moravec";

/** @brief Application directory used by `UserDataDirectory`. Matches the GUI application identity. */
inline constexpr std::string_view USER_DATA_APPLICATION_NAME = "StructuredLogViewer";

/**
 * @brief Value of environment variable @p name as a path.
 *
 * Windows reads the wide-character environment, so names outside the
 * active code page survive. Call it at startup, before other threads
 * change the environment.
 *
 * @param name ASCII variable name.
 * @return The value, or `std::nullopt` when the variable is unset or empty.
 */
[[nodiscard]] std::optional<std::filesystem::path> EnvironmentPath(std::string_view name);

/**
 * @brief Whether environment variable @p name exists, even with an empty value.
 *
 * @param name ASCII variable name.
 */
[[nodiscard]] bool EnvironmentIsSet(std::string_view name);

/**
 * @brief Writable application-data directory, matching `QStandardPaths::AppDataLocation`.
 *
 * Windows uses `%APPDATA%/<organization>/<application>` and returns an
 * empty path when `%APPDATA%` is unset. Linux uses
 * `$XDG_DATA_HOME/<organization>/<application>` when that variable is
 * absolute, and `~/.local/share/<organization>/<application>` otherwise.
 * A relative `XDG_DATA_HOME` is ignored. macOS uses
 * `~/Library/Application Support/<organization>/<application>`.
 * The organization and application segments are
 * `USER_DATA_ORGANIZATION_NAME` and `USER_DATA_APPLICATION_NAME`.
 */
[[nodiscard]] std::filesystem::path UserDataDirectory();

/**
 * @brief `UserDataDirectory() / "regex_templates"`.
 * Empty when `UserDataDirectory()` is empty. Does not create the directory.
 */
[[nodiscard]] std::filesystem::path UserRegexTemplateDirectory();

/**
 * @brief Templates loaded from `*.json` files in @p directory, plus per-file errors.
 *
 * Files are read in filename order. A missing `priority` keeps
 * `USER_TEMPLATE_DEFAULT_PRIORITY` through `ParseRegexTemplate`. A file
 * that cannot be read or parsed is skipped; its message is appended to
 * `errors` and the walk continues.
 */
struct RegexTemplateLoadResult
{
    std::vector<RegexTemplate> templates;
    std::vector<std::string> errors;
};

/**
 * @brief Load every `*.json` regex template in @p directory.
 *
 * A missing directory yields an empty result. The directory itself is
 * not created.
 *
 * @param directory Folder of user template JSON files.
 * @return Parsed templates and one error string per skipped file.
 */
[[nodiscard]] RegexTemplateLoadResult LoadRegexTemplatesFromDirectory(const std::filesystem::path &directory);

} // namespace loglib
