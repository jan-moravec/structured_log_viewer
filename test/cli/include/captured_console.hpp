#pragma once

#include "console.hpp"
#include "diagnostics.hpp"
#include "options.hpp"

#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>

namespace slp::test
{

/** @brief Opens an anonymous read-write temporary file, or returns null. */
[[nodiscard]] inline std::FILE *OpenTemporaryFile() noexcept
{
#ifdef _WIN32
    std::FILE *file = nullptr;
    return tmpfile_s(&file) == 0 ? file : nullptr;
#else
    return std::tmpfile();
#endif
}

/** @brief What one in-process command wrote, and its exit code. */
struct Captured
{
    int exitCode = 0;
    std::string out;
    std::string err;
};

/**
 * @brief Runs @p run with a `Console` whose streams are temporary files.
 *
 * @param options Parsed invocation that names the command.
 * @param run Callable taking `slp::Console &` and returning `slp::ExitCode`.
 * @return The exit code and both streams' text.
 */
template <typename Run> [[nodiscard]] Captured Capture(const Options &options, Run run)
{
    const auto closer = [](std::FILE *file) { (void)std::fclose(file); };
    const std::unique_ptr<std::FILE, decltype(closer)> out(OpenTemporaryFile(), closer);
    const std::unique_ptr<std::FILE, decltype(closer)> err(OpenTemporaryFile(), closer);
    if (!out || !err)
    {
        throw std::runtime_error("could not create temporary capture files");
    }

    Console console(out.get(), err.get(), CommandName(options.command), options.global);
    Captured captured;
    captured.exitCode = static_cast<int>(run(console));

    const auto readBack = [](std::FILE *file) {
        std::string text;
        if (std::fseek(file, 0, SEEK_SET) != 0)
        {
            throw std::runtime_error("could not rewind a capture file");
        }
        char buffer[4096];
        std::size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) > 0)
        {
            text.append(buffer, read);
        }
        return text;
    };
    captured.out = readBack(out.get());
    captured.err = readBack(err.get());
    return captured;
}

} // namespace slp::test
