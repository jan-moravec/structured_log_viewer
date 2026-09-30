#pragma once

#include "loglib/regex_templates.hpp"

#include <filesystem>
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
