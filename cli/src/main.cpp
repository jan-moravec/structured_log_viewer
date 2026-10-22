#include "commands.hpp"
#include "console.hpp"
#include "diagnostics.hpp"
#include "options.hpp"
#include "platform.hpp"

#include <loglib/decompressing_byte_source.hpp>
#include <loglib/exports/export_sink.hpp>
#include <loglib/exports/row_exporter.hpp>
#include <loglib/session_bundle.hpp>

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <system_error>

namespace
{

class ConsoleRestore
{
public:
    ConsoleRestore() = default;
    ~ConsoleRestore()
    {
        slp::RestoreConsoleOutput();
    }
    ConsoleRestore(const ConsoleRestore &) = delete;
    ConsoleRestore &operator=(const ConsoleRestore &) = delete;
};

[[nodiscard]] std::filesystem::path AppImageDir()
{
#if !defined(_WIN32) && !defined(__APPLE__)
    if (const char *value = std::getenv("APPDIR"); value != nullptr && value[0] != '\0')
    {
        return std::filesystem::path(value);
    }
#endif
    return {};
}

[[nodiscard]] int Exit(slp::ExitCode code) noexcept
{
    return static_cast<int>(code);
}

} // namespace

int main(int argc, char **argv)
{
    slp::PrepareStandardStreams();
    const ConsoleRestore restoreConsole;
    loglib::StopSource stop;
    slp::InstallSignalHandlers(stop);

    try
    {
        const auto parsed = slp::ParseArguments(argc, argv);
        if (!parsed)
        {
            slp::WriteUsage(parsed.error());
            return Exit(slp::ExitCode::Usage);
        }
        switch (parsed->kind)
        {
        case slp::ParseKind::Help:
            std::cout << parsed->helpText;
            return Exit(slp::ExitCode::Success);
        case slp::ParseKind::Version:
            std::cout << slp::VersionLine() << '\n';
            return Exit(slp::ExitCode::Success);
        case slp::ParseKind::MissingCommand:
            std::cout << parsed->helpText;
            return Exit(slp::ExitCode::Usage);
        case slp::ParseKind::Run:
            break;
        }

        std::vector<std::filesystem::path> searched;
        std::error_code cwdError;
        const std::filesystem::path cwd = std::filesystem::current_path(cwdError);
        const std::filesystem::path tzdata = slp::FindTzdataDirectory(
            slp::CurrentExecutable().parent_path(), cwdError ? std::filesystem::path{} : cwd, AppImageDir(), searched
        );
        const slp::TimeZoneSetup zone = slp::ResolveTimeZone(parsed->global.timeZone, tzdata);
        if (!zone.error.empty())
        {
            slp::WriteError({}, zone.error);
            return Exit(slp::ExitCode::Usage);
        }
        if (!zone.warning.empty())
        {
            slp::Console(stdout, stderr, {}, parsed->global).Warn(zone.warning);
        }
        loglib::SetProcessDefaultTimeZone(zone.context);
        const slp::RuntimeContext runtime{.stop = stop.get_token(), .timeZone = zone.context};
        return slp::Execute(*parsed, runtime);
    }
    catch (const loglib::exports::BrokenPipe &)
    {
        return Exit(slp::ExitCode::Success);
    }
    catch (const loglib::DecompressionCancelled &)
    {
        return Exit(slp::ExitCode::Cancelled);
    }
    catch (const loglib::exports::ExportCancelled &)
    {
        return Exit(slp::ExitCode::Cancelled);
    }
    catch (const loglib::SessionBundleCancelled &)
    {
        return Exit(slp::ExitCode::Cancelled);
    }
    catch (const std::exception &ex)
    {
        // Commands map expected input and output failures to their own
        // exit codes; anything that reaches here is a defect in slp.
        slp::WriteError({}, std::format("internal error: {}", ex.what()));
        return Exit(slp::ExitCode::Internal);
    }
    catch (...)
    {
        slp::WriteError({}, "internal error: an unexpected exception was thrown");
        return Exit(slp::ExitCode::Internal);
    }
}
