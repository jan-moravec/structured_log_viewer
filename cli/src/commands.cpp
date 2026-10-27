#include "commands.hpp"

#include "console.hpp"

#include <variant>

namespace slp
{

namespace
{

ExitCode RunCommand(
    const QueryOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
)
{
    return RunQuery(options, global, console, runtime);
}

ExitCode RunCommand(
    const StatsOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
)
{
    return RunStats(options, global, console, runtime);
}

ExitCode RunCommand(
    const SchemaOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
)
{
    return RunSchema(options, global, console, runtime);
}

ExitCode RunCommand(
    const ValidateOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
)
{
    return RunValidate(options, global, console, runtime);
}

ExitCode RunCommand(
    const BundleOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
)
{
    return RunBundle(options, global, console, runtime);
}

ExitCode RunCommand(
    const FormatsOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext & /*runtime*/
)
{
    return RunFormats(options, global, console);
}

ExitCode RunCommand(
    const TailOptions &options, const GlobalOptions &global, Console &console, const RuntimeContext &runtime
)
{
    return RunTail(options, global, console, runtime);
}

} // namespace

int Execute(const Options &options, const RuntimeContext &runtime)
{
    Console console(stdout, stderr, CommandName(options.command), options.global);
    return static_cast<int>(std::visit(
        [&](const auto &command) { return RunCommand(command, options.global, console, runtime); }, options.command
    ));
}

} // namespace slp
