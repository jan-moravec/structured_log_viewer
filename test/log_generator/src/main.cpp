// Standalone JSONL log generator. Drives `test_common::GenerateRandomJsonLogLine`
// in a loop, writing one record per output line until either a target byte
// count or a target line count is reached. Supports streaming throttling via
// `--timeout` and in-flight file rotation (`rename` / `copytruncate` /
// `truncate`) via the `--roll-*` flags so it can drive `TailingBytesProducer`
// rotation tests and the GUI's Stream
// Mode smoke tests end-to-end.
//
// Each emitted record is stamped with a monotonic, session-global,
// 0-based line index under the `line_number` field. The counter is
// unaffected by rotation, so a consumer that observes a gap in the
// sequence can conclude that a line was dropped end-to-end (across the
// producer's `write()`, the rotation handler, the `TailingBytesProducer`
// partial-line buffer, and the GUI thread queue).

#include <test_common/json_log_line.hpp>
#include <test_common/log_generator.hpp>

#include <argparse/argparse.hpp>
#include <glaze/glaze.hpp>

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <variant>

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

// Parse a line-count literal: plain integer or with `K` / `M` / `G` suffix
// (base 1000, case-insensitive). Distinct from `ParseSize` because line
// counts conventionally use SI multipliers (`100K` = 100'000, not 102'400).
std::uint64_t ParseCount(std::string text)
{
    if (text.empty())
    {
        throw std::invalid_argument("count must not be empty");
    }

    auto upper = text;
    for (char &c : upper)
    {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }

    std::uint64_t multiplier = 1;
    if (!upper.empty() && upper.back() == 'K')
    {
        multiplier = 1000ULL;
        upper.pop_back();
    }
    else if (!upper.empty() && upper.back() == 'M')
    {
        multiplier = 1000ULL * 1000ULL;
        upper.pop_back();
    }
    else if (!upper.empty() && upper.back() == 'G')
    {
        multiplier = 1000ULL * 1000ULL * 1000ULL;
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
        throw std::invalid_argument("count must contain a numeric value");
    }
    for (char c : upper)
    {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0)
        {
            throw std::invalid_argument("count '" + text + "' is not a valid integer with optional K/M/G suffix");
        }
    }

    return std::stoull(upper) * multiplier;
}

enum class RollStrategy
{
    /// `mv path -> path.1`, then create a fresh `path`. Triggers
    /// `TailingBytesProducer` rotation branch (i) -- identity change.
    /// The default and the most realistic logrotate emulation.
    Rename,
    /// `cp path -> path.1`, then truncate `path` in place. Triggers
    /// rotation branch (iii) — size shrunk. Models the
    /// `copytruncate` logrotate option used when the producer cannot be
    /// signalled to reopen.
    CopyTruncate,
    /// Truncate `path` in place with no backup. Also triggers branch
    /// (iii); useful for stress-testing the partial-line-buffer discard
    /// path on rotation.
    Truncate,
};

RollStrategy ParseRollStrategy(const std::string &text)
{
    if (text == "rename")
    {
        return RollStrategy::Rename;
    }
    if (text == "copytruncate")
    {
        return RollStrategy::CopyTruncate;
    }
    if (text == "truncate")
    {
        return RollStrategy::Truncate;
    }
    throw std::invalid_argument("unknown --roll-strategy '" + text + "' (expected rename|copytruncate|truncate)");
}

// `<base>.<n>` — same naming convention as logrotate (`app.log.1`,
// `app.log.2`, ...). Kept as a free function so both the shift and the
// rotation routines can use it without a shared state object.
std::filesystem::path NumberedBackup(const std::filesystem::path &basePath, std::size_t n)
{
    std::filesystem::path p = basePath;
    p += "." + std::to_string(n);
    return p;
}

// Shift the existing backups one slot up: `path.{N-1}` -> `path.N`,
// `path.{N-2}` -> `path.{N-1}`, ..., freeing slot `path.1` for the next
// rotation. The oldest backup (`path.N`) is unlinked. Errors are
// swallowed via `std::error_code` because rotation must not fail just
// because a backup slot did not exist on the previous run.
void ShiftBackups(const std::filesystem::path &basePath, std::size_t keepRolled)
{
    if (keepRolled == 0)
    {
        return;
    }

    std::error_code ec;
    std::filesystem::remove(NumberedBackup(basePath, keepRolled), ec);

    for (std::size_t i = keepRolled; i >= 2; --i)
    {
        const auto from = NumberedBackup(basePath, i - 1);
        const auto to = NumberedBackup(basePath, i);
        if (std::filesystem::exists(from, ec))
        {
            std::filesystem::rename(from, to, ec);
        }
    }
}

// Rotate the active output, leaving `out` open at offset 0 of a freshly
// (re-)created `basePath`. The caller is expected to reset its
// per-file byte / line counters after this returns.
//
// Failure modes: a closed `out` after this call signals that the
// re-open step failed; the caller should log and exit. We deliberately
// use exceptions for the filesystem step so the `Rotate` caller can
// bail cleanly rather than silently producing garbled output.
void Rotate(std::ofstream &out, const std::filesystem::path &basePath, RollStrategy strategy, std::size_t keepRolled)
{
    out.flush();
    out.close();

    switch (strategy)
    {
    case RollStrategy::Rename:
    {
        if (keepRolled > 0)
        {
            ShiftBackups(basePath, keepRolled);
            // `rename` may fail on Windows if `path.1` still exists
            // (rare, since we just shifted it away); use the error_code
            // overload to decide whether to fall back to copy+remove.
            std::error_code ec;
            std::filesystem::rename(basePath, NumberedBackup(basePath, 1), ec);
            if (ec)
            {
                // Best-effort fallback: copy the active file to slot .1
                // and unlink the original. Keeps tests deterministic
                // even on filesystems that block in-use renames.
                std::filesystem::copy_file(
                    basePath, NumberedBackup(basePath, 1), std::filesystem::copy_options::overwrite_existing
                );
                std::filesystem::remove(basePath);
            }
        }
        else
        {
            std::error_code ec;
            std::filesystem::remove(basePath, ec);
        }
        out.open(basePath, std::ios::binary | std::ios::trunc);
        break;
    }
    case RollStrategy::CopyTruncate:
    {
        if (keepRolled > 0)
        {
            ShiftBackups(basePath, keepRolled);
            std::error_code ec;
            std::filesystem::copy_file(
                basePath, NumberedBackup(basePath, 1), std::filesystem::copy_options::overwrite_existing, ec
            );
            // A failure here is non-fatal: the backup is best-effort,
            // but the in-place truncate still has to happen so the
            // tailing consumer observes the size-shrunk signal.
        }
        // In-place truncate via re-open with `trunc`: the dropped
        // content is what triggers `TailingBytesProducer` branch (iii).
        out.open(basePath, std::ios::binary | std::ios::trunc);
        break;
    }
    case RollStrategy::Truncate:
    {
        out.open(basePath, std::ios::binary | std::ios::trunc);
        break;
    }
    }
}

} // namespace

int main(int argc, char *argv[])
{
    argparse::ArgumentParser program("log_generator", "0.2.0");
    program.add_description("Generate a JSONL log file with synthetic timestamp/level/message records. "
                            "Lines are produced until --size or --lines is reached (whichever comes first; "
                            "0 means unbounded on that axis). Pass --timeout to throttle writes and simulate "
                            "a streaming feed; pass --roll-size and/or --roll-lines to rotate the active file "
                            "in-flight (--roll-strategy controls how).");

    program.add_argument("-s", "--size")
        .default_value(std::string{"10MB"})
        .help("Total output budget in bytes across all rolls. Plain bytes or with suffix B/KB/MB/GB (base 1024). 0 = "
              "unbounded.");

    program.add_argument("-n", "--lines")
        .default_value(std::string{"0"})
        .help("Total output budget in lines across all rolls. Plain integer or with suffix K/M/G (base 1000). 0 = "
              "unbounded.");

    program.add_argument("-o", "--output")
        .default_value(std::string{"generated.jsonl"})
        .help("Output file path (overwritten if it already exists, unless --append).");

    program.add_argument("-t", "--timeout")
        .default_value(0)
        .scan<'i', int>()
        .help("Delay in milliseconds between line writes. 0 disables throttling.");

    program.add_argument("--seed").scan<'i', int>().help(
        "Optional RNG seed for reproducible output. Defaults to std::random_device."
    );

    program.add_argument("--append")
        .default_value(false)
        .implicit_value(true)
        .help("Append to an existing output file rather than truncating at start.");

    program.add_argument("--roll-size")
        .default_value(std::string{"0"})
        .help("Rotate the active file once it reaches at least this size (B/KB/MB/GB suffix supported, base 1024). 0 "
              "disables size-triggered rotation.");

    program.add_argument("--roll-lines")
        .default_value(std::string{"0"})
        .help("Rotate the active file once it has at least this many lines (K/M/G suffix supported, base 1000). 0 "
              "disables line-triggered rotation.");

    program.add_argument("--roll-strategy")
        .default_value(std::string{"rename"})
        .choices("rename", "copytruncate", "truncate")
        .help("Rotation strategy. 'rename' (mv path -> path.1, recreate path;  identity change), "
              "'copytruncate' (cp path path.1, then truncate path in place;  size shrunk), "
              "'truncate' (in-place truncate, no backup;  size shrunk).");

    program.add_argument("--keep-rolled")
        .default_value(5)
        .scan<'i', int>()
        .help(
            "Number of rotated backups to keep at <output>.1 .. <output>.N. 0 deletes content on rotation (no backup)."
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
    const auto linesText = program.get<std::string>("--lines");
    const auto outputPath = std::filesystem::path(program.get<std::string>("--output"));
    const auto timeoutMs = program.get<int>("--timeout");
    const auto append = program.get<bool>("--append");
    const auto rollSizeText = program.get<std::string>("--roll-size");
    const auto rollLinesText = program.get<std::string>("--roll-lines");
    const auto rollStrategyText = program.get<std::string>("--roll-strategy");
    const auto keepRolledInt = program.get<int>("--keep-rolled");
    const std::uint32_t seed = program.is_used("--seed") ? static_cast<std::uint32_t>(program.get<int>("--seed"))
                                                         : test_common::MakeRandomSeed();

    std::uint64_t targetBytes = 0;
    std::uint64_t targetLines = 0;
    std::uint64_t rollBytes = 0;
    std::uint64_t rollLines = 0;
    RollStrategy strategy{RollStrategy::Rename};
    try
    {
        targetBytes = ParseSize(sizeText);
        targetLines = ParseCount(linesText);
        rollBytes = ParseSize(rollSizeText);
        rollLines = ParseCount(rollLinesText);
        strategy = ParseRollStrategy(rollStrategyText);
    }
    catch (const std::exception &err)
    {
        std::cerr << "Invalid argument: " << err.what() << '\n' << program;
        return 1;
    }

    if (timeoutMs < 0)
    {
        std::cerr << "Invalid --timeout: must be >= 0\n" << program;
        return 1;
    }
    if (keepRolledInt < 0)
    {
        std::cerr << "Invalid --keep-rolled: must be >= 0\n" << program;
        return 1;
    }
    const std::size_t keepRolled = static_cast<std::size_t>(keepRolledInt);

    const bool rollingEnabled = rollBytes != 0 || rollLines != 0;

    const auto openMode = std::ios::binary | (append ? std::ios::app : std::ios::trunc);
    std::ofstream out(outputPath, openMode);
    if (!out.is_open())
    {
        std::cerr << "Failed to open output file: " << outputPath << '\n';
        return 1;
    }

    // Seed `bytesInFile` from the existing on-disk size when --append
    // is in effect so a subsequent --roll-size trigger considers what
    // is already on disk. Line count cannot be cheaply derived without
    // re-reading the file, so we leave it at 0 — `--roll-lines` after
    // `--append` is therefore measured from the appended-only segment;
    // call it a documented quirk rather than a bug.
    std::uint64_t bytesInFile = 0;
    if (append)
    {
        std::error_code ec;
        const auto existingSize = std::filesystem::file_size(outputPath, ec);
        if (!ec)
        {
            bytesInFile = static_cast<std::uint64_t>(existingSize);
        }
    }

    std::cout << "log_generator: writing"
              << " up to "
              << (targetBytes == 0 ? std::string{"unbounded bytes"} : std::to_string(targetBytes) + " bytes") << ", "
              << (targetLines == 0 ? std::string{"unbounded lines"} : std::to_string(targetLines) + " lines") << " to "
              << outputPath << " (timeout=" << timeoutMs << "ms, seed=" << seed
              << ", append=" << (append ? "true" : "false");
    if (rollingEnabled)
    {
        std::cout << ", roll=" << rollStrategyText << " every "
                  << (rollBytes == 0 ? std::string{"-"} : std::to_string(rollBytes) + "B") << "/"
                  << (rollLines == 0 ? std::string{"-"} : std::to_string(rollLines) + " lines") << ", keep "
                  << keepRolled << " backups";
    }
    std::cout << ")\n";

    if (targetBytes == 0 && targetLines == 0)
    {
        std::cout << "log_generator: both --size and --lines are 0; running until interrupted (Ctrl+C).\n";
    }

    std::mt19937 rng(seed);
    std::uint64_t totalBytes = 0;
    std::uint64_t totalLines = 0;
    std::uint64_t linesInFile = 0;
    std::size_t rotationCount = 0;
    const auto start = std::chrono::steady_clock::now();

    const auto reachedTotal = [&]() {
        if (targetBytes != 0 && totalBytes >= targetBytes)
        {
            return true;
        }
        if (targetLines != 0 && totalLines >= targetLines)
        {
            return true;
        }
        return false;
    };
    const auto shouldRoll = [&]() {
        if (!rollingEnabled)
        {
            return false;
        }
        if (rollBytes != 0 && bytesInFile >= rollBytes)
        {
            return true;
        }
        if (rollLines != 0 && linesInFile >= rollLines)
        {
            return true;
        }
        return false;
    };

    while (!reachedTotal())
    {
        auto line = test_common::GenerateRandomJsonLogLine(rng, totalLines);
        // Stamp the JSON object with the session-global line index so a
        // tailing consumer can detect dropped rows by observing gaps in
        // the sequence. `GenerateRandomJsonLogLine` always returns the
        // `glz::generic_sorted_u64` variant; the `get_if` is defensive.
        if (auto *json = std::get_if<glz::generic_sorted_u64>(&line.data))
        {
            (*json)["line_number"] = static_cast<std::int64_t>(totalLines);
        }
        const std::string serialized = line.ToString();
        out << serialized << '\n';
        if (!out.good())
        {
            std::cerr << "Write failed at byte " << totalBytes << '\n';
            return 1;
        }
        const std::uint64_t bytesWritten = serialized.size() + 1U;
        totalBytes += bytesWritten;
        bytesInFile += bytesWritten;
        ++totalLines;
        ++linesInFile;

        if (timeoutMs > 0)
        {
            // Flush so the consumer can see incremental progress while we sleep;
            // without this the stdlib buffer would hide everything until close.
            out.flush();
            std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        }

        if (shouldRoll() && !reachedTotal())
        {
            try
            {
                Rotate(out, outputPath, strategy, keepRolled);
            }
            catch (const std::exception &err)
            {
                std::cerr << "Rotation failed: " << err.what() << '\n';
                return 1;
            }
            if (!out.is_open())
            {
                std::cerr << "Failed to re-open output file after rotation: " << outputPath << '\n';
                return 1;
            }
            ++rotationCount;
            bytesInFile = 0;
            linesInFile = 0;
            std::cout << "log_generator: rotated #" << rotationCount << " (" << rollStrategyText
                      << "), continuing into " << outputPath << " (total so far: " << totalLines << " lines, "
                      << totalBytes << " bytes)\n";
        }
    }

    out.flush();
    out.close();

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    std::cout << "log_generator: wrote " << totalLines << " lines, " << totalBytes << " bytes in " << elapsedMs
              << " ms (" << rotationCount << " rotation" << (rotationCount == 1 ? "" : "s") << ", " << outputPath
              << ")\n";
    return 0;
}
