// Standalone log generator. Loops `test_common::GenerateRandomLogRecord`,
// serializes each record through `--format` (`json` / `logfmt`), and stops
// at `--size` or `--lines`. Supports `--timeout` throttling and in-flight
// rotation (`--roll-*`) for `TailingBytesProducer` and Stream Mode smoke
// tests.
//
// The format's `writeHeader` is emitted on initial open (skipped in
// `--append`) and after every rotation, so a future schema-bearing format
// drops in without re-plumbing. JSON / logfmt return an empty header.
//
// Default output is file (`--target file://...` or legacy `--output`).
// Network targets:
//
//   * `tcp://host:port`     plaintext TCP -> `TcpServerProducer`
//   * `tcp+tls://host:port` TLS-on-TCP; use `--tls-ca` (verify), or
//                           `--tls-skip-verify` (self-signed dev), or
//                           `--tls-cert` / `--tls-key` (mTLS).
//   * `udp://host:port`     UDP datagram per line -> `UdpServerProducer`.
//
// Every record carries a `line_number` field (0-based, monotonic,
// rotation-agnostic); gaps in the sequence at the consumer indicate
// end-to-end drops.

#include <test_common/log_format.hpp>
#include <test_common/log_generator.hpp>
#include <test_common/log_record.hpp>
#include <test_common/network_log_client.hpp>

#include <argparse/argparse.hpp>

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

namespace
{

// Parse a byte-count literal: plain integer or with `B`/`KB`/`MB`/`GB`
// suffix (case-insensitive, base 1024).
std::uint64_t ParseSize(const std::string &text)
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
    for (const char c : upper)
    {
        if (std::isdigit(static_cast<unsigned char>(c)) == 0)
        {
            throw std::invalid_argument("size '" + text + "' is not a valid integer with optional KB/MB/GB suffix");
        }
    }

    const std::uint64_t value = std::stoull(upper);
    return value * multiplier;
}

// Parse a line-count literal: plain integer or with `K`/`M`/`G` suffix
// (case-insensitive, base 1000 — line counts use SI multipliers).
std::uint64_t ParseCount(const std::string &text)
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
    for (const char c : upper)
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
    /// `TailingBytesProducer` branch (i) — identity change. Default;
    /// most realistic logrotate emulation.
    Rename,
    /// `cp path -> path.1`, then truncate `path` in place. Triggers
    /// branch (iii) — size shrunk. Models logrotate's `copytruncate`.
    CopyTruncate,
    /// In-place truncate, no backup. Also triggers branch (iii);
    /// useful for the partial-line-buffer discard path.
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

enum class TargetKind
{
    File,
    Tcp,
    TcpTls,
    Udp,
};

struct ParsedTarget
{
    TargetKind kind = TargetKind::File;
    std::string filePath;   // valid for File
    std::string host;       // valid for Tcp/TcpTls/Udp
    std::uint16_t port = 0; // valid for Tcp/TcpTls/Udp
};

// Parse a URL-style target. Throws on malformed network URLs rather than
// silently falling back to file mode.
ParsedTarget ParseTarget(const std::string &target)
{
    auto schemeAt = target.find("://");
    if (schemeAt == std::string::npos)
    {
        // No scheme -> treat as a bare file path.
        ParsedTarget out;
        out.kind = TargetKind::File;
        out.filePath = target;
        return out;
    }

    const std::string scheme = target.substr(0, schemeAt);
    const std::string rest = target.substr(schemeAt + 3);

    if (scheme == "file")
    {
        // Strip the leading slash from Windows-style `file:///C:/foo`.
        ParsedTarget out;
        out.kind = TargetKind::File;
        if (!rest.empty() && rest.front() == '/' && rest.size() >= 3 &&
            std::isalpha(static_cast<unsigned char>(rest[1])) && rest[2] == ':')
        {
            out.filePath = rest.substr(1);
        }
        else
        {
            out.filePath = rest;
        }
        return out;
    }

    TargetKind kind = TargetKind::File;
    if (scheme == "tcp")
    {
        kind = TargetKind::Tcp;
    }
    else if (scheme == "tcp+tls")
    {
        kind = TargetKind::TcpTls;
    }
    else if (scheme == "udp")
    {
        kind = TargetKind::Udp;
    }
    else
    {
        throw std::invalid_argument(
            "unknown --target scheme '" + scheme + "' (expected file://, tcp://, tcp+tls://, udp://)"
        );
    }

    // Strip any path component after host:port (network targets ignore it).
    auto pathAt = rest.find('/');
    const std::string hostPort = (pathAt == std::string::npos) ? rest : rest.substr(0, pathAt);

    // Split host:port; bracketed IPv6 literals like `[::1]:5141` are supported.
    std::string hostText;
    std::string portText;
    if (!hostPort.empty() && hostPort.front() == '[')
    {
        const auto closeBracket = hostPort.find(']');
        if (closeBracket == std::string::npos)
        {
            throw std::invalid_argument("malformed IPv6 literal in --target '" + target + "'");
        }
        hostText = hostPort.substr(1, closeBracket - 1);
        if (closeBracket + 1 >= hostPort.size() || hostPort[closeBracket + 1] != ':')
        {
            throw std::invalid_argument("--target '" + target + "' must include :PORT after the IPv6 literal");
        }
        portText = hostPort.substr(closeBracket + 2);
    }
    else
    {
        const auto colon = hostPort.rfind(':');
        if (colon == std::string::npos)
        {
            throw std::invalid_argument("--target '" + target + "' must include :PORT");
        }
        hostText = hostPort.substr(0, colon);
        portText = hostPort.substr(colon + 1);
    }

    if (hostText.empty() || portText.empty())
    {
        throw std::invalid_argument("--target '" + target + "' must be of the form scheme://HOST:PORT");
    }
    int port = 0;
    try
    {
        port = std::stoi(portText);
    }
    catch (const std::exception &)
    {
        throw std::invalid_argument("--target '" + target + "' has a non-numeric port");
    }
    if (port <= 0 || port > 65535)
    {
        throw std::invalid_argument("--target '" + target + "' port must be in 1..65535");
    }

    ParsedTarget out;
    out.kind = kind;
    out.host = hostText;
    out.port = static_cast<std::uint16_t>(port);
    return out;
}

// `<base>.<n>` — logrotate naming convention (`app.log.1`, `app.log.2`, …).
std::filesystem::path NumberedBackup(const std::filesystem::path &basePath, std::size_t n)
{
    std::filesystem::path p = basePath;
    p += "." + std::to_string(n);
    return p;
}

// Shift backups up one slot (`path.{N-1}` -> `path.N`, …), freeing
// `path.1` for the next rotation. The oldest (`path.N`) is unlinked.
// Errors are swallowed because rotation must not fail just because a
// backup slot was missing.
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

// Emit the format header to a freshly opened `out` and return the bytes
// written (0 for self-describing formats). Must run on every fresh file
// (initial open, post-rotation re-open, copytruncate).
std::uint64_t WriteFormatHeader(
    std::ofstream &out, const test_common::LogFormat &format, const test_common::RecordSchema &schema
)
{
    const std::string header = format.writeHeader(schema);
    if (header.empty())
    {
        return 0;
    }
    out << header << '\n';
    return static_cast<std::uint64_t>(header.size()) + 1U;
}

// Rotate the active output: leave `out` open at offset 0 of a freshly
// (re-)created `basePath` with the format header re-emitted. Returns the
// header byte count so the caller can reseed its per-file accounting.
// A closed `out` on return signals the re-open failed. Filesystem errors
// surface as exceptions so the caller can bail cleanly.
std::uint64_t Rotate(
    std::ofstream &out, const std::filesystem::path &basePath, RollStrategy strategy, std::size_t keepRolled,
    const test_common::LogFormat &format, const test_common::RecordSchema &schema
)
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
            // Use the error_code overload so we can fall back to
            // copy+remove if `rename` fails (e.g. Windows in-use file).
            std::error_code ec;
            std::filesystem::rename(basePath, NumberedBackup(basePath, 1), ec);
            if (ec)
            {
                // Copy-and-unlink fallback so tests stay deterministic
                // on filesystems that block in-use renames.
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
            // Backup failure is non-fatal; the truncate below still has
            // to happen so the tailing consumer sees the size-shrunk signal.
        }
        // In-place truncate -> triggers `TailingBytesProducer` branch (iii).
        out.open(basePath, std::ios::binary | std::ios::trunc);
        break;
    }
    case RollStrategy::Truncate:
    {
        out.open(basePath, std::ios::binary | std::ios::trunc);
        break;
    }
    }

    if (!out.is_open())
    {
        return 0;
    }
    return WriteFormatHeader(out, format, schema);
}

} // namespace

// CLI tool; `ParseSize` and argparse surface errors via exceptions by design.
// NOLINTNEXTLINE(bugprone-exception-escape)
int main(int argc, char *argv[])
{
    argparse::ArgumentParser program("log_generator", "0.3.0");
    program.add_description("Generate a structured log file with synthetic timestamp/level/message records. "
                            "Pick the wire format with --format (json|logfmt). Lines are produced until --size "
                            "or --lines is reached (whichever comes first; 0 means unbounded on that axis). "
                            "Pass --timeout to throttle writes and simulate a streaming feed; pass --roll-size "
                            "and/or --roll-lines to rotate the active file in-flight (--roll-strategy controls "
                            "how). When --output is omitted the default base name takes the format's extension "
                            "(generated.jsonl for json, generated.logfmt for logfmt).");

    program.add_argument("-s", "--size")
        .default_value(std::string{"10MB"})
        .help("Total output budget in bytes across all rolls. Plain bytes or with suffix B/KB/MB/GB (base 1024). 0 = "
              "unbounded.");

    program.add_argument("-n", "--lines")
        .default_value(std::string{"0"})
        .help("Total output budget in lines across all rolls. Plain integer or with suffix K/M/G (base 1000). 0 = "
              "unbounded.");

    program.add_argument("-o", "--output")
        .default_value(std::string{})
        .help("Output file path (overwritten if it already exists, unless --append). When --output is "
              "omitted the default base name is `generated` plus the format's extension "
              "(generated.jsonl for --format json, generated.logfmt for --format logfmt).");

    program.add_argument("-f", "--format")
        .default_value(std::string{"json"})
        .choices("json", "logfmt")
        .help("Record serialization format: 'json' (one JSON object per line) or 'logfmt'.");

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

    program.add_argument("--target")
        .help("URL-style output destination. Supersedes --output when set. "
              "Examples: file:///tmp/foo.jsonl, tcp://127.0.0.1:5141, "
              "tcp+tls://example:6514, udp://127.0.0.1:5142. When unset, "
              "--output is used (file mode). The --roll-* flags require "
              "file mode and are rejected for network targets.");

    program.add_argument("--tls-ca")
        .help("Optional PEM CA bundle used to verify the server certificate (tcp+tls:// only).");
    program.add_argument("--tls-cert").help("Optional client certificate PEM for mutual TLS (tcp+tls:// only).");
    program.add_argument("--tls-key").help("Optional client private key PEM matching --tls-cert (tcp+tls:// only).");
    program.add_argument("--tls-skip-verify")
        .default_value(false)
        .implicit_value(true)
        .help("Skip server certificate validation. Required for self-signed dev certs; never use in production.");
    program.add_argument("--tls-sni")
        .help("Override the SNI hostname sent during the handshake. Defaults to the URL host.");

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
    const auto formatName = program.get<std::string>("--format");
    // The dispatch table is the source of truth — adding a new `--format`
    // choice without updating it must fail loudly here, not fall through.
    test_common::LogFormat format;
    if (formatName == "json")
    {
        format = test_common::JsonLines();
    }
    else if (formatName == "logfmt")
    {
        format = test_common::Logfmt();
    }
    else
    {
        std::cerr << "Unknown --format value: " << formatName << " (expected one of: json, logfmt)\n";
        return 1;
    }
    // No schema-bearing format ships today; the empty schema is a hook so
    // a future CSV-like format only needs to extend the dispatch table.
    const test_common::RecordSchema schema;
    // Empty `--output` means "derive from the format" (`generated.jsonl` /
    // `generated.logfmt`); the argparse default is empty so `--help` doesn't
    // bias toward JSON.
    auto outputArg = program.get<std::string>("--output");
    if (outputArg.empty())
    {
        outputArg = "generated" + std::string(format.suggestedExtension);
    }
    const auto outputPath = std::filesystem::path(outputArg);
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
    ParsedTarget target;
    target.kind = TargetKind::File;
    target.filePath = outputPath.string();
    try
    {
        targetBytes = ParseSize(sizeText);
        targetLines = ParseCount(linesText);
        rollBytes = ParseSize(rollSizeText);
        rollLines = ParseCount(rollLinesText);
        strategy = ParseRollStrategy(rollStrategyText);
        if (program.is_used("--target"))
        {
            target = ParseTarget(program.get<std::string>("--target"));
        }
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
    const auto keepRolled = static_cast<std::size_t>(keepRolledInt);

    const bool rollingEnabled = rollBytes != 0 || rollLines != 0;
    const bool isNetworkTarget =
        target.kind == TargetKind::Tcp || target.kind == TargetKind::TcpTls || target.kind == TargetKind::Udp;

    if (isNetworkTarget && rollingEnabled)
    {
        std::cerr << "--roll-size / --roll-lines require a file target; rotation is not meaningful for "
                     "tcp/udp streams.\n";
        return 1;
    }
    if (isNetworkTarget && append)
    {
        std::cerr << "--append requires a file target.\n";
        return 1;
    }

    // Network sinks. At most one is non-null; both null means file mode.
    std::unique_ptr<test_common::TcpLogClient> tcpClient;
    std::unique_ptr<test_common::UdpLogClient> udpClient;

    // File sink. Populated only in file mode.
    std::ofstream out;
    std::filesystem::path filePath;

    std::string targetDescription;
    // Header bytes (0 for self-describing formats), folded into `bytesInFile`
    // so `--roll-size` counts the header like any other byte.
    std::uint64_t headerBytes = 0;
    if (target.kind == TargetKind::File)
    {
        filePath = std::filesystem::path(target.filePath);
        const auto openMode = std::ios::binary | (append ? std::ios::app : std::ios::trunc);
        out.open(filePath, openMode);
        if (!out.is_open())
        {
            std::cerr << "Failed to open output file: " << filePath << '\n';
            return 1;
        }
        // Skip the header in --append mode; the existing file is assumed
        // to already have one and re-emitting it would corrupt the file.
        if (!append)
        {
            headerBytes = WriteFormatHeader(out, format, schema);
        }
        targetDescription = "file://" + filePath.string();
    }
    else if (target.kind == TargetKind::Udp)
    {
        try
        {
            udpClient = std::make_unique<test_common::UdpLogClient>(target.host, target.port);
        }
        catch (const std::exception &err)
        {
            std::cerr << "Failed to create UDP client: " << err.what() << '\n';
            return 1;
        }
        targetDescription = "udp://" + target.host + ":" + std::to_string(target.port);
    }
    else // Tcp or TcpTls
    {
        std::optional<test_common::TcpLogClient::TlsOptions> tls;
        if (target.kind == TargetKind::TcpTls)
        {
            tls.emplace();
            if (program.is_used("--tls-ca"))
            {
                tls->caBundle = std::filesystem::path(program.get<std::string>("--tls-ca"));
            }
            if (program.is_used("--tls-cert"))
            {
                tls->clientCert = std::filesystem::path(program.get<std::string>("--tls-cert"));
            }
            if (program.is_used("--tls-key"))
            {
                tls->clientKey = std::filesystem::path(program.get<std::string>("--tls-key"));
            }
            tls->insecureSkipVerify = program.get<bool>("--tls-skip-verify");
            if (program.is_used("--tls-sni"))
            {
                tls->serverNameIndication = program.get<std::string>("--tls-sni");
            }
        }
        try
        {
            tcpClient = std::make_unique<test_common::TcpLogClient>(target.host, target.port, std::move(tls));
        }
        catch (const std::exception &err)
        {
            std::cerr << "Failed to connect to TCP target: " << err.what() << '\n';
            return 1;
        }
        targetDescription = (target.kind == TargetKind::TcpTls ? "tcp+tls://" : "tcp://") + target.host + ":" +
                            std::to_string(target.port);
    }

    // Seed `bytesInFile` from the existing file size on --append so
    // --roll-size considers what's already on disk. Line count can't be
    // cheaply recovered, so --roll-lines after --append measures only the
    // appended segment (documented quirk).
    std::uint64_t bytesInFile = 0;
    if (append && target.kind == TargetKind::File)
    {
        std::error_code ec;
        const auto existingSize = std::filesystem::file_size(filePath, ec);
        if (!ec)
        {
            bytesInFile = static_cast<std::uint64_t>(existingSize);
        }
    }
    bytesInFile += headerBytes;

    std::cout << "log_generator: writing"
              << " up to "
              << (targetBytes == 0 ? std::string{"unbounded bytes"} : std::to_string(targetBytes) + " bytes") << ", "
              << (targetLines == 0 ? std::string{"unbounded lines"} : std::to_string(targetLines) + " lines") << " to "
              << targetDescription << " (format=" << formatName << ", timeout=" << timeoutMs << "ms, seed=" << seed
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
        test_common::LogRecord record = test_common::GenerateRandomLogRecord(rng, totalLines);
        // Session-global, rotation-agnostic counter so a tailing consumer
        // can detect drops by spotting gaps in the sequence.
        record["line_number"] = static_cast<std::int64_t>(totalLines);
        const std::string serialized = format.writeLine(record);

        if (target.kind == TargetKind::File)
        {
            out << serialized << '\n';
            if (!out.good())
            {
                std::cerr << "Write failed at byte " << totalBytes << '\n';
                return 1;
            }
        }
        else if (udpClient)
        {
            try
            {
                udpClient->Send(serialized);
            }
            catch (const std::exception &err)
            {
                std::cerr << "UDP send failed at line " << totalLines << ": " << err.what() << '\n';
                return 1;
            }
        }
        else if (tcpClient)
        {
            try
            {
                tcpClient->Send(serialized);
            }
            catch (const std::exception &err)
            {
                std::cerr << "TCP send failed at line " << totalLines << ": " << err.what() << '\n';
                return 1;
            }
        }

        const std::uint64_t bytesWritten = serialized.size() + 1U;
        totalBytes += bytesWritten;
        bytesInFile += bytesWritten;
        ++totalLines;
        ++linesInFile;

        if (timeoutMs > 0)
        {
            // Flush so consumers see incremental progress during the sleep.
            // Network clients write synchronously and need no extra flush.
            if (target.kind == TargetKind::File)
            {
                out.flush();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(timeoutMs));
        }

        if (shouldRoll() && !reachedTotal())
        {
            std::uint64_t headerBytesAfterRotate = 0;
            try
            {
                headerBytesAfterRotate = Rotate(out, filePath, strategy, keepRolled, format, schema);
            }
            catch (const std::exception &err)
            {
                std::cerr << "Rotation failed: " << err.what() << '\n';
                return 1;
            }
            if (!out.is_open())
            {
                std::cerr << "Failed to re-open output file after rotation: " << filePath << '\n';
                return 1;
            }
            ++rotationCount;
            // Reset per-file counters. `bytesInFile` includes the rewritten
            // header so `--roll-size` still accounts for it; `totalBytes`
            // intentionally tracks only random-record bytes for the summary.
            bytesInFile = headerBytesAfterRotate;
            linesInFile = 0;
            std::cout << "log_generator: rotated #" << rotationCount << " (" << rollStrategyText
                      << "), continuing into " << filePath << " (total so far: " << totalLines << " lines, "
                      << totalBytes << " bytes)\n";
        }
    }

    if (target.kind == TargetKind::File)
    {
        out.flush();
        out.close();
    }
    if (tcpClient)
    {
        tcpClient->Close();
    }
    if (udpClient)
    {
        udpClient->Close();
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    std::cout << "log_generator: wrote " << totalLines << " lines, " << totalBytes << " bytes in " << elapsedMs
              << " ms (" << rotationCount << " rotation" << (rotationCount == 1 ? "" : "s") << ", " << targetDescription
              << ")\n";
    return 0;
}
