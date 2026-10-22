#include "commands.hpp"
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
            return static_cast<int>(slp::ExitCode::Usage);
        }
        if (parsed->kind == slp::ParseKind::Help)
        {
            std::cout << parsed->helpText;
            return static_cast<int>(slp::ExitCode::Success);
        }
        if (parsed->kind == slp::ParseKind::Version)
        {
            std::cout << slp::VersionLine() << '\n';
            return static_cast<int>(slp::ExitCode::Success);
        }
        if (parsed->kind == slp::ParseKind::MissingCommand)
        {
            std::cout << parsed->helpText;
            return static_cast<int>(slp::ExitCode::Usage);
        }

        std::vector<std::filesystem::path> searched;
        std::error_code cwdError;
        const std::filesystem::path cwd = std::filesystem::current_path(cwdError);
        const std::filesystem::path tzdata = slp::FindTzdataDirectory(
            slp::CurrentExecutable().parent_path(), cwdError ? std::filesystem::path{} : cwd, AppImageDir(), searched
        );
        const slp::TimeZoneSetup zone = slp::ResolveTimeZone(parsed->timeZone, tzdata);
        if (!zone.error.empty())
        {
            slp::WriteError({}, zone.error);
            return static_cast<int>(slp::ExitCode::Usage);
        }
        if (!zone.warning.empty())
        {
            slp::WriteWarning(parsed->quiet, zone.warning);
        }
        loglib::SetProcessDefaultTimeZone(zone.context);
        const slp::RuntimeContext runtime{.stop = stop.get_token(), .timeZone = zone.context};
        return slp::Execute(*parsed, runtime);
    }
    catch (const loglib::exports::BrokenPipe &)
    {
        return static_cast<int>(slp::ExitCode::Success);
    }
    catch (const loglib::DecompressionCancelled &)
    {
        return static_cast<int>(slp::ExitCode::Cancelled);
    }
    catch (const loglib::DecompressionSizeCapExceeded &ex)
    {
        slp::WriteError({}, ex.what());
        return static_cast<int>(slp::ExitCode::Input);
    }
    catch (const loglib::SessionBundleVersionError &ex)
    {
        slp::WriteError({}, ex.what());
        return static_cast<int>(slp::ExitCode::Input);
    }
    catch (const loglib::SessionBundleReadError &ex)
    {
        slp::WriteError({}, ex.what());
        return static_cast<int>(slp::ExitCode::Input);
    }
    catch (const loglib::exports::ExportCancelled &)
    {
        return static_cast<int>(slp::ExitCode::Cancelled);
    }
    catch (const loglib::SessionBundleCancelled &)
    {
        return static_cast<int>(slp::ExitCode::Cancelled);
    }
    catch (const std::runtime_error &ex)
    {
        slp::WriteError({}, ex.what());
        return static_cast<int>(slp::ExitCode::Output);
    }
    catch (const std::exception &ex)
    {
        slp::WriteError({}, ex.what());
        return static_cast<int>(slp::ExitCode::Output);
    }
    catch (...)
    {
        slp::WriteError({}, "an unexpected error occurred");
        return static_cast<int>(slp::ExitCode::Output);
    }
}
