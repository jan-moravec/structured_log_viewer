#include "commands.hpp"

#include "console.hpp"

#include <variant>

namespace slp
{

namespace
{

/** @brief Calls the `Run*` function that matches each options type. */
struct Dispatch
{
    const GlobalOptions &global;
    Console &console;
    const RuntimeContext &runtime;

    ExitCode operator()(const QueryOptions &options) const
    {
        return RunQuery(options, global, console, runtime);
    }
    ExitCode operator()(const StatsOptions &options) const
    {
        return RunStats(options, global, console, runtime);
    }
    ExitCode operator()(const SchemaOptions &options) const
    {
        return RunSchema(options, global, console, runtime);
    }
    ExitCode operator()(const ValidateOptions &options) const
    {
        return RunValidate(options, global, console, runtime);
    }
    ExitCode operator()(const BundleOptions &options) const
    {
        return RunBundle(options, global, console, runtime);
    }
    ExitCode operator()(const FormatsOptions &options) const
    {
        return RunFormats(options, global, console);
    }
    ExitCode operator()(const TailOptions &options) const
    {
        return RunTail(options, global, console, runtime);
    }
};

} // namespace

int Execute(const Options &options, const RuntimeContext &runtime)
{
    Console console(stdout, stderr, CommandName(options.command), options.global);
    return static_cast<int>(std::visit(Dispatch{options.global, console, runtime}, options.command));
}

} // namespace slp
