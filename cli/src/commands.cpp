#include "commands.hpp"

#include "diagnostics.hpp"

#include <iostream>
#include <string_view>

namespace slp
{

namespace
{

[[nodiscard]] std::string_view Name(Command command) noexcept
{
    switch (command)
    {
    case Command::Query:
        return "query";
    case Command::Stats:
        return "stats";
    case Command::Schema:
        return "schema";
    case Command::Validate:
        return "validate";
    case Command::Bundle:
        return "bundle";
    case Command::Formats:
        return "formats";
    case Command::Tail:
        return "tail";
    case Command::None:
        return {};
    }
    return {};
}

} // namespace

int Execute(const Options &options, const RuntimeContext &runtime)
{
    if (options.command == Command::Formats)
    {
        const CommandOutput rendered = RenderFormats(options);
        std::cout << rendered.out;
        std::cerr << rendered.err;
        return rendered.exitCode;
    }
    if (options.command == Command::Query)
    {
        const CommandOutput rendered = RunQuery(options, runtime);
        // Row bytes for `-` go through StreamSink(stdout) inside RunQuery.
        // Only `--count` (and similar) leave payload in `out`.
        std::cout << rendered.out;
        std::cerr << rendered.err;
        return rendered.exitCode;
    }
    if (options.command == Command::Schema)
    {
        const CommandOutput rendered = RunSchema(options, runtime);
        std::cout << rendered.out;
        std::cerr << rendered.err;
        return rendered.exitCode;
    }
    if (options.command == Command::Validate)
    {
        const CommandOutput rendered = RunValidate(options, runtime);
        std::cout << rendered.out;
        std::cerr << rendered.err;
        return rendered.exitCode;
    }
    if (options.command == Command::Bundle)
    {
        const CommandOutput rendered = RunBundle(options, runtime);
        std::cout << rendered.out;
        std::cerr << rendered.err;
        return rendered.exitCode;
    }
    if (options.command == Command::Tail)
    {
        const CommandOutput rendered = RunTail(options, runtime);
        std::cout << rendered.out;
        std::cerr << rendered.err;
        return rendered.exitCode;
    }
    if (options.command == Command::Stats)
    {
        const CommandOutput rendered = RunStats(options, runtime);
        std::cout << rendered.out;
        std::cerr << rendered.err;
        return rendered.exitCode;
    }

    WriteError(Name(options.command), "this command is not implemented yet");
    return static_cast<int>(ExitCode::Usage);
}

} // namespace slp
