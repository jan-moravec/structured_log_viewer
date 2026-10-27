#include "console.hpp"

#include "platform.hpp"

#include <loglib/exports/export_sink.hpp>

#include <format>
#include <stdexcept>

namespace slp
{

Console::Console(std::FILE *out, std::FILE *err, std::string_view command, const GlobalOptions &global)
    : mOut(out), mErr(err), mCommand(command), mQuiet(global.quiet), mVerbose(global.verbose), mColor(global.color)
{
}

bool Console::ColorFor(std::FILE *stream) const noexcept
{
    return mColor == ColorMode::Always || (mColor == ColorMode::Auto && IsTerminal(stream));
}

ExitCode Console::Print(std::string_view text) const
{
    if (text.empty())
    {
        return ExitCode::Success;
    }
    try
    {
        loglib::exports::StreamSink sink(mOut);
        sink.Write(text);
        sink.Finish();
    }
    catch (const std::runtime_error &ex)
    {
        return Report(Fail(ExitCode::Output, std::format("could not write to standard output: {}", ex.what())));
    }
    return ExitCode::Success;
}

void Console::Error(std::string_view sentence) const
{
    WriteErr(FormatErrorLine(mCommand, sentence) + '\n');
}

void Console::Warn(std::string_view sentence) const
{
    if (!mQuiet)
    {
        WriteErr(FormatWarningLine(sentence) + '\n');
    }
}

void Console::Note(std::string_view text) const
{
    if (!mQuiet)
    {
        WriteErr(text);
    }
}

void Console::Stage(std::string_view stage, std::uint64_t rows, std::chrono::steady_clock::time_point started) const
{
    if (mVerbose <= 0)
    {
        return;
    }
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    WriteErr(FormatStageLine(stage, rows, static_cast<std::uint64_t>(elapsed.count())) + '\n');
}

void Console::ParseErrors(const std::vector<std::string> &errors) const
{
    if (errors.empty() || mQuiet)
    {
        return;
    }
    WriteErr(
        FormatErrorLine({}, std::format("{} lines could not be parsed (use --verbose to list)", errors.size())) + '\n'
    );
    if (mVerbose > 0)
    {
        for (const std::string &error : errors)
        {
            WriteErr(FormatWarningLine(error) + '\n');
        }
    }
}

ExitCode Console::Report(const Failure &failure) const
{
    WriteErr(failure.text);
    return failure.code;
}

Failure Console::Fail(ExitCode code, std::string_view sentence) const
{
    return MakeFailure(code, mCommand, sentence);
}

void Console::WriteErr(std::string_view text) const
{
    if (!text.empty())
    {
        (void)std::fwrite(text.data(), 1, text.size(), mErr);
        (void)std::fflush(mErr);
    }
}

} // namespace slp
