#include "diagnostics.hpp"

#include <iostream>
#include <string>

namespace slp
{

namespace
{

constexpr std::string_view EXIT_CODE_HELP =
    "Exit codes:\n"
    "  0    Success.\n"
    "  1    The command ran, but the result set was empty. --allow-empty turns this into 0.\n"
    "  2    Usage error: unknown flag, bad value, malformed filter, or unresolvable column.\n"
    "  3    Input error: the source is missing, unreadable, undetectable, or failed to decompress.\n"
    "  4    Data error: parse errors under --strict, or validate found errors.\n"
    "  5    Output error: the destination is unwritable, exists without --force, or the disk is full.\n"
    "  70   Internal error: an unexpected failure inside slp. Please report it.\n"
    "  130  Cancelled by SIGINT.\n"
    "\n"
    "Sources concatenate in the order given. Rotated siblings load oldest first with the named file last.\n"
    "Use --sort with a time column when the output must be chronological.\n";

std::string Prefix(std::string_view command)
{
    std::string line = "slp";
    if (!command.empty())
    {
        line.push_back(' ');
        line.append(command);
    }
    line.append(": ");
    return line;
}

} // namespace

Failure MakeFailure(ExitCode code, std::string_view command, std::string_view sentence)
{
    Failure failure{.code = code, .text = FormatErrorLine(command, sentence)};
    failure.text.push_back('\n');
    return failure;
}

Failure CancelledFailure()
{
    return Failure{.code = ExitCode::Cancelled, .text = {}};
}

std::string_view ExitCodeHelpEpilog() noexcept
{
    return EXIT_CODE_HELP;
}

std::string FormatErrorLine(std::string_view command, std::string_view sentence)
{
    std::string line = Prefix(command);
    line.append(sentence);
    if (line.empty() || line.back() != '.')
    {
        line.push_back('.');
    }
    return line;
}

std::string FormatWarningLine(std::string_view sentence)
{
    return FormatErrorLine({}, sentence);
}

std::string FormatStageLine(std::string_view stage, std::uint64_t rows, std::uint64_t milliseconds)
{
    return "stage=" + std::string(stage) + " rows=" + std::to_string(rows) + " ms=" + std::to_string(milliseconds);
}

void WriteUsage(const UsageError &error)
{
    std::cerr << FormatErrorLine(error.command, error.message) << '\n';
    if (!error.usage.empty())
    {
        std::cerr << error.usage << '\n';
    }
}

void WriteError(std::string_view command, std::string_view sentence)
{
    std::cerr << FormatErrorLine(command, sentence) << '\n';
}

std::string FormatFilterParseDiagnostic(std::string_view query, std::size_t offset, std::string_view message)
{
    std::string text = FormatErrorLine({}, "invalid --filter expression");
    text.push_back('\n');
    text.append("  ");
    text.append(query);
    text.push_back('\n');
    text.append("  ");
    const std::size_t caret = offset <= query.size() ? offset : query.size();
    text.append(caret, ' ');
    text.push_back('^');
    text.push_back('\n');
    text.append("  ");
    text.append(message);
    if (text.empty() || text.back() != '.')
    {
        text.push_back('.');
    }
    return text;
}

} // namespace slp
