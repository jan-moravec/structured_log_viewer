#pragma once

#include "diagnostics.hpp"
#include "options.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace slp
{

/**
 * @brief The output and diagnostic streams of one command.
 *
 * Every write goes to its stream at once, so warnings and verbose stage
 * lines appear while the command runs, in order with any progress output.
 * Warnings and notes are dropped under `--quiet`; errors never are. Tests
 * pass temporary files in place of stdout and stderr.
 */
class Console
{
public:
    /**
     * @param out Payload stream, normally stdout.
     * @param err Diagnostic stream, normally stderr.
     * @param command Subcommand name used in error prefixes.
     * @param global Resolved global flags.
     */
    Console(std::FILE *out, std::FILE *err, std::string_view command, const GlobalOptions &global);

    /** @brief Payload stream for commands that stream rows through an export sink. */
    [[nodiscard]] std::FILE *Out() const noexcept
    {
        return mOut;
    }

    /** @brief Diagnostic stream, for in-place progress lines. */
    [[nodiscard]] std::FILE *Err() const noexcept
    {
        return mErr;
    }

    [[nodiscard]] std::string_view Command() const noexcept
    {
        return mCommand;
    }

    [[nodiscard]] bool Quiet() const noexcept
    {
        return mQuiet;
    }

    [[nodiscard]] int Verbose() const noexcept
    {
        return mVerbose;
    }

    /** @brief Whether human-readable output to @p stream is colored under `--color`. */
    [[nodiscard]] bool ColorFor(std::FILE *stream) const noexcept;

    /** @brief The resolved `--color` mode. */
    [[nodiscard]] ColorMode Color() const noexcept
    {
        return mColor;
    }

    /** @brief Writes payload text to the output stream. */
    void Print(std::string_view text) const;

    /** @brief Writes `slp <command>: <sentence>.` to the diagnostic stream. */
    void Error(std::string_view sentence) const;

    /** @brief Writes `slp: <sentence>.` unless `--quiet` is set. */
    void Warn(std::string_view sentence) const;

    /** @brief Writes @p text as is unless `--quiet` is set. */
    void Note(std::string_view text) const;

    /** @brief Writes a stage line under `--verbose`. */
    void Stage(std::string_view stage, std::uint64_t rows, std::chrono::steady_clock::time_point started) const;

    /**
     * @brief Reports parse errors: a count line, then each error under `--verbose`.
     *
     * Nothing is written for no errors or under `--quiet`.
     */
    void ParseErrors(const std::vector<std::string> &errors) const;

    /** @brief Writes @p failure's text and returns its exit code. */
    ExitCode Report(const Failure &failure) const;

    /** @brief Builds a failure prefixed with this console's command. */
    [[nodiscard]] Failure Fail(ExitCode code, std::string_view sentence) const;

private:
    void WriteErr(std::string_view text) const;

    std::FILE *mOut;
    std::FILE *mErr;
    std::string mCommand;
    bool mQuiet;
    int mVerbose;
    ColorMode mColor;
};

} // namespace slp
