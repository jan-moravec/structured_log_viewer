#include <log_generator/formats.hpp>

#include <test_common/log_generator.hpp>

#include <array>
#include <cctype>
#include <cstddef>
#include <random>

namespace log_generator
{

namespace
{

// Adding a regex-template synthesizer means adding a row here plus its
// factory in `test/common/src/log_format.cpp`. Names stay `[a-z0-9-]+`
// because `DefaultFileName` inlines them into a file name.
constexpr std::array<FormatSpec, 8> FORMATS = {
    FormatSpec{
        .name = "json",
        .family = FormatFamily::Structured,
        .description = "One JSON object per line (default)",
        .needsSchema = false,
        .make = [](const test_common::RecordSchema &) { return test_common::JsonLines(); },
    },
    FormatSpec{
        .name = "logfmt",
        .family = FormatFamily::Structured,
        .description = "key=value pairs, quoted where needed",
        .needsSchema = false,
        .make = [](const test_common::RecordSchema &) { return test_common::Logfmt(); },
    },
    FormatSpec{
        .name = "csv",
        .family = FormatFamily::Structured,
        .description = "RFC 4180, comma-separated, with a header row",
        .needsSchema = true,
        .make = [](const test_common::RecordSchema &schema) { return test_common::Csv(schema); },
    },
    FormatSpec{
        .name = "syslog",
        .family = FormatFamily::RegexTemplate,
        .templateName = "Syslog (RFC3164)",
        .description = "BSD syslog: `MMM DD HH:MM:SS <host> <program>[<pid>]: <message>`",
        .needsSchema = false,
        .make = [](const test_common::RecordSchema &) { return test_common::SyslogRfc3164Format(); },
    },
    FormatSpec{
        .name = "apache-combined",
        .family = FormatFamily::RegexTemplate,
        .templateName = "Apache/nginx Combined Log Format",
        .description = R"(Apache/nginx access log with "<referrer>" "<agent>")",
        .needsSchema = false,
        .make = [](const test_common::RecordSchema &) { return test_common::ApacheCombinedFormat(); },
    },
    FormatSpec{
        .name = "apache-common",
        .family = FormatFamily::RegexTemplate,
        .templateName = "Apache/nginx Common Log Format",
        .description = "Apache/nginx access log without referrer and agent",
        .needsSchema = false,
        .make = [](const test_common::RecordSchema &) { return test_common::ApacheCommonFormat(); },
    },
    FormatSpec{
        .name = "apache-error",
        .family = FormatFamily::RegexTemplate,
        .templateName = "Apache error log",
        .description = "Apache 2.4 error log: `[<time>] [<mod>:<lvl>] [pid N] [client ip:port] <msg>`",
        .needsSchema = false,
        .make = [](const test_common::RecordSchema &) { return test_common::ApacheErrorFormat(); },
    },
    FormatSpec{
        .name = "java",
        .family = FormatFamily::RegexTemplate,
        .templateName = "Java / log4j / SLF4J Logback",
        .description = "Logback / log4j / SLF4J: `<time> LEVEL [<thread>] <logger> - <msg>`",
        .needsSchema = false,
        .make = [](const test_common::RecordSchema &) { return test_common::JavaLogFormat(); },
    },
};

bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept
{
    if (lhs.size() != rhs.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < lhs.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(lhs[i])) != std::tolower(static_cast<unsigned char>(rhs[i])))
        {
            return false;
        }
    }
    return true;
}

} // namespace

std::span<const FormatSpec> Formats() noexcept
{
    return FORMATS;
}

const FormatSpec *FindFormat(std::string_view name) noexcept
{
    for (const auto &spec : FORMATS)
    {
        if (EqualsIgnoreCase(name, spec.name))
        {
            return &spec;
        }
    }
    return nullptr;
}

void PrintFormatList(std::ostream &os)
{
    const auto printFamily = [&os](FormatFamily family) {
        for (const auto &spec : FORMATS)
        {
            if (spec.family != family)
            {
                continue;
            }
            os << "  " << spec.name << std::string(spec.name.size() < 17 ? 17 - spec.name.size() : 1, ' ')
               << spec.description << '\n';
            if (!spec.templateName.empty())
            {
                os << std::string(19, ' ') << "parsed by the \"" << spec.templateName << "\" template\n";
            }
        }
    };
    os << "Structured formats:\n";
    printFamily(FormatFamily::Structured);
    os << "\nRegex-template formats:\n";
    printFamily(FormatFamily::RegexTemplate);
}

std::string DefaultFileName(const FormatSpec &spec)
{
    const std::string extension(spec.make({}).suggestedExtension);
    if (spec.family == FormatFamily::RegexTemplate)
    {
        return "generated_" + std::string(spec.name) + extension;
    }
    return "generated" + extension;
}

ResolvedFormat ResolveFormat(const FormatSpec &spec, std::uint32_t seed)
{
    ResolvedFormat resolved;
    if (spec.needsSchema)
    {
        // Every generated record has the same keys, so one probe record on a
        // throwaway RNG gives the header. `line_number` is added per line.
        std::mt19937 probeRng(seed);
        test_common::LogRecord probe = test_common::GenerateRandomLogRecord(probeRng, 0);
        probe["line_number"] = static_cast<std::int64_t>(0);
        resolved.schema = test_common::DeriveSchemaFromRecord(probe);
    }
    resolved.format = spec.make(resolved.schema);
    return resolved;
}

} // namespace log_generator
