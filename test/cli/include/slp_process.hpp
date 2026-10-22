#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace slp::test
{

/** @brief Captured stdout, stderr, and exit code from one `slp` run. */
struct CommandResult
{
    int exitCode = -1;
    std::string out;
    std::string err;
};

/** @brief Absolute path of the built `slp` binary. */
[[nodiscard]] std::filesystem::path SlpBinary();

/** @brief Absolute path of `test/cli/fixtures`. */
[[nodiscard]] std::filesystem::path FixturesDir();

/** @brief Absolute path of `test/cli/golden`. */
[[nodiscard]] std::filesystem::path GoldenDir();

/** @brief Spawns `slp` with @p args and captures both streams. */
[[nodiscard]] CommandResult RunSlp(const std::vector<std::string> &args);

/**
 * @brief Asserts @p actual matches the golden file at @p relativeName.
 *
 * When `SLP_UPDATE_GOLDEN` is set in the environment, writes @p actual
 * to the golden path instead of comparing. On mismatch, the failure
 * message names the first differing byte offset.
 */
void RequireGolden(std::string_view relativeName, std::string_view actual);

/**
 * @brief Returns a short human-readable diff for two byte strings.
 *
 * Names the first differing offset and shows a short context window.
 */
[[nodiscard]] std::string DescribeByteDiff(std::string_view expected, std::string_view actual);

} // namespace slp::test
