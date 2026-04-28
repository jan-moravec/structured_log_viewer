// Standalone JSONL log generator. Drives `test_common::GenerateRandomJsonLogLine`
// in a loop, writing one record per output line until a target byte count is
// reached. Useful for producing fixture files for manual UI smoke tests and for
// simulating a streaming feed via `--timeout`.

#include <test_common/json_log_line.hpp>
#include <test_common/log_generator.hpp>

#include <argparse/argparse.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{

// Parse a size literal that accepts a plain byte count (`10485760`) or one of
// the common suffixes `B`, `KB`, `MB`, `GB` (case-insensitive, base 1024).
// We deliberately keep this in one TU rather than pulling in fmt/std::regex
// because the `log_generator` binary should stay tiny.
std::uint64_t ParseSize(std::string text)
{
    if (text.empty())
    {
        throw std::invalid_argument("size must not be empty");
    }

    auto upper = text;
    for (char &c : upper)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    std::uint64_t multiplier = 1;
    if (upper.size() >= 2 && upper.substr(upper.size() - 2) == "KB")
    {
        multiplier = 1024ULL;
        upper.resize(upper.size() - 2);
    }
    else if (upper.size() >= 2 && upper.substr(upper.size() - 2) == "MB")
    {
        multiplier = 1024ULL * 1024ULL;
        upper.resize(upper.size() - 2);
    }
    else if (upper.size() >= 2 && upper.substr(upper.size() - 2) == "GB")
    {
        multiplier = 1024ULL * 1024ULL * 1024ULL;
        upper.resize(upper.size() - 2);
    }
    else if (!upper.empty() && upper.back() == 'B')
    {
        upper.pop_back();
    }

    while (!upper.empty() && std::isspace(static_cast<unsigned char>(upper.back())) != 0)
    {
        upper.pop_back();
    }
    while (!upper.empty() && std::isspace(static_cast<unsigned char>(upper.front())) != 0)
    {
        upper.erase(upper.begin());
    }

    if (upper.empty())
    {
        throw std::invalid_argument("size must contain a numeric value");
    }
    for (char c : upper)
    {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0)
        {
            throw std::invalid_argument("size '" + text + "' is not a valid integer with optional KB/MB/GB suffix");
        }
    }

    const std::uint64_t value = std::stoull(upper);
    return value * multiplier;
}

} // namespace

int main(int argc, char *argv[])
{
    argparse::ArgumentParser program("log_generator", "0.1.0");
    program.add_description("Generate a JSONL log file with synthetic timestamp/level/message records. "
                            "Lines are produced until the file reaches the requested size; pass --timeout "
                            "to throttle writes and simulate a streaming feed.");

    program.add_argument("-s", "--size")
        .default_value(std::string{"10MB"})
        .help("Target output size. Plain bytes or with suffix B/KB/MB/GB (base 1024).");

    program.add_argument("-o", "--output")
        .default_value(std::string{"generated.jsonl"})
        .help("Output file path (overwritten if it already exists).");

    program.add_argument("-t", "--timeout")
        .default_value(0)
        .scan<'i', int>()
        .help("Delay in milliseconds between line writes. 0 disables throttling.");

    program.add_argument("--seed").scan<'i', int>().help(
        "Optional RNG seed for reproducible output. Defaults to std::random_device."
    );

    try
    {
        program.parse_args(argc, argv);
    }
    catch (const std::exception &err)
    {
        std::cerr << err.what() << '\n' << program;
        return 1;
    }

    const auto sizeText = program.get<std::string>("--size");
    const auto outputPath = program.get<std::string>("--output");
    const auto timeoutMs = program.get<int>("--timeout");
    const std::uint32_t seed = program.is_used("--seed") ? static_cast<std::uint32_t>(program.get<int>("--seed"))
                                                         : test_common::MakeRandomSeed();

    std::uint64_t targetBytes = 0;
    try
    {
        targetBytes = ParseSize(sizeText);
    }
    catch (const std::exception &err)
    {
        std::cerr << "Invalid --size: " << err.what() << '\n' << program;
        return 1;
    }

    if (timeoutMs < 0)
    {
        std::cerr << "Invalid --timeout: must be >= 0\n" << program;
        return 1;
    }

    std::ofstream out(outputPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
    {
        std::cerr << "Failed to open output file: " << outputPath << '\n';
        return 1;
    }

    std::cout << "log_generator: writing up to " << targetBytes << " bytes to " << outputPath
              << " (timeout=" << timeoutMs << "ms, seed=" << seed << ")\n";

    std::mt19937 rng(seed);
    std::uint64_t bytesWritten = 0;
    std::size_t linesWritten = 0;
    const auto start = std::chrono::steady_clock::now();

    while (bytesWritten < targetBytes)
    {
        const auto line = test_common::GenerateRandomJsonLogLine(rng, linesWritten);
        const std::string serialized = line.ToString();
        out << serialized << '\n';
        if (!out.good())
        {
            std::cerr << "Write failed at byte " << bytesWritten << '\n';
            return 1;
        }
        bytesWritten += static_cast<std::uint64_t>(serialized.size()) + 1U;
        ++linesWritten;

        if (timeoutMs > 0)
        {
            // Flush so the consumer can see incremental progress while we sleep;
            // without this the stdlib buffer would hide everything until close.
            out.flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        }
    }

    out.flush();
    out.close();

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    std::cout << "log_generator: wrote " << linesWritten << " lines, " << bytesWritten << " bytes in " << elapsedMs
              << " ms (" << outputPath << ")\n";
    return 0;
}
