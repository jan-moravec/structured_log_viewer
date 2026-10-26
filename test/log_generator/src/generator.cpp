#include <log_generator/generator.hpp>

#include <log_generator/formats.hpp>
#include <log_generator/pacer.hpp>
#include <log_generator/sinks.hpp>

#include <test_common/log_generator.hpp>

#include <date/date.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <string>

namespace log_generator
{

namespace
{

std::string DescribeLimits(const Limits &limits)
{
    if (limits.forever)
    {
        return "until interrupted";
    }
    std::string text;
    const auto add = [&text](const std::string &part) { text += (text.empty() ? "up to " : ", ") + part; };
    if (limits.lines.has_value())
    {
        add(std::to_string(*limits.lines) + " lines");
    }
    if (limits.bytes.has_value())
    {
        add(std::to_string(*limits.bytes) + " bytes");
    }
    if (limits.duration.has_value())
    {
        add(std::to_string(limits.duration->count()) + " ms");
    }
    return text;
}

bool LimitReached(const Limits &limits, std::uint64_t lines, std::uint64_t bytes)
{
    return (limits.lines.has_value() && lines >= *limits.lines) || (limits.bytes.has_value() && bytes >= *limits.bytes);
}

} // namespace

void Run(const GeneratorOptions &options)
{
    std::ofstream nullSink;
    // Keep progress messages out of a stdout record stream.
    const auto selectDiag = [&]() -> std::ostream & {
        if (options.quiet)
        {
            return nullSink;
        }
        return options.target.kind == TargetKind::Stdout ? std::cerr : std::cout;
    };
    std::ostream &diag = selectDiag();

    const std::uint32_t seed = options.seed.value_or(test_common::MakeRandomSeed());
    const ResolvedFormat format = ResolveFormat(*options.format, seed);
    const std::unique_ptr<Sink> sink = OpenSink(options, format.Header(), diag);
    Pacer pacer(options.timing, options.limits.duration);

    diag << "log_generator: writing " << DescribeLimits(options.limits) << " to " << options.target.Describe()
         << " (format=" << options.format->name << ", seed=" << seed << ", " << pacer.Describe();
    if (options.append)
    {
        diag << ", append";
    }
    if (options.rotation.Enabled())
    {
        diag << ", " << ToString(options.rotation.strategy) << " every "
             << (options.rotation.maxBytes == 0 ? std::string{"-"} : std::to_string(options.rotation.maxBytes) + "B")
             << "/"
             << (options.rotation.maxLines == 0 ? std::string{"-"}
                                                : std::to_string(options.rotation.maxLines) + " lines")
             << ", keep " << options.rotation.keep;
    }
    diag << ")\n";

    std::mt19937 rng(seed);
    std::uint64_t lines = 0;
    std::uint64_t bytes = 0;
    const auto start = std::chrono::steady_clock::now();
    while (!LimitReached(options.limits, lines, bytes))
    {
        test_common::LogRecord record = test_common::GenerateRandomLogRecord(rng, lines);
        if (!pacer.Stamp(record, rng))
        {
            break;
        }
        record["line_number"] = static_cast<std::int64_t>(lines);
        const std::string serialized = format.format.writeLine(record);
        if (pacer.Paced())
        {
            // Let readers see what was written so far before sleeping.
            sink->Flush();
            pacer.WaitForStamp();
        }
        if (!sink->WriteLine(serialized))
        {
            break;
        }
        bytes += serialized.size() + 1U;
        ++lines;
    }
    sink->Close();

    const auto elapsedMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
    const std::size_t rotations = sink->Rotations();
    diag << "log_generator: wrote " << lines << " lines, " << bytes << " bytes in " << elapsedMs << " ms (" << rotations
         << " rotation" << (rotations == 1 ? "" : "s") << ", " << options.target.Describe() << ")\n";
    if (const auto simEnd = pacer.SimulatedEnd(); simEnd.has_value())
    {
        diag << "log_generator: simulated span "
             << date::format("%FT%T", date::floor<std::chrono::seconds>(*pacer.SimulatedStart())) << " .. "
             << date::format("%FT%T", date::floor<std::chrono::seconds>(*simEnd)) << " UTC\n";
    }
}

} // namespace log_generator
