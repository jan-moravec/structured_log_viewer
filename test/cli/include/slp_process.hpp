#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
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
 * @brief Spawns `slp` with @p args, feeds @p stdinBytes to the child's
 * standard input, and captures both output streams.
 */
[[nodiscard]] CommandResult RunSlpWithStdin(const std::vector<std::string> &args, std::string_view stdinBytes);

/**
 * @brief A running `slp` child whose output is read while it runs.
 *
 * Standard input is empty. Both output streams are captured in temporary
 * files that the destructor removes. A child still running at
 * destruction is killed.
 */
class SlpChild
{
public:
    /** @brief Starts `slp` with @p args. */
    explicit SlpChild(const std::vector<std::string> &args);
    ~SlpChild();

    SlpChild(const SlpChild &) = delete;
    SlpChild &operator=(const SlpChild &) = delete;
    SlpChild(SlpChild &&) = delete;
    SlpChild &operator=(SlpChild &&) = delete;

    /** @brief Polls stderr until it contains @p needle; false on timeout. */
    [[nodiscard]] bool WaitForErr(std::string_view needle, std::chrono::milliseconds timeout) const;

    /** @brief Polls stdout until it contains @p needle; false on timeout. */
    [[nodiscard]] bool WaitForOut(std::string_view needle, std::chrono::milliseconds timeout) const;

    /** @brief Waits for exit and returns the exit code, or nullopt on timeout. */
    [[nodiscard]] std::optional<int> Wait(std::chrono::milliseconds timeout);

    /** @brief Sends `SIGINT` to the child. Returns false where the platform cannot deliver it. */
    [[nodiscard]] bool Interrupt();

    /** @brief Standard output captured so far. */
    [[nodiscard]] std::string Out() const;

    /** @brief Standard error captured so far. */
    [[nodiscard]] std::string Err() const;

private:
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

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
