#pragma once

#include <log_generator/formats.hpp>
#include <log_generator/rotation.hpp>
#include <log_generator/target.hpp>

#include <test_common/arrival_clock.hpp>
#include <test_common/network_log_client.hpp>

#include <CLI/CLI.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace log_generator
{

/** @brief Byte limit applied when no limit is given on the command line. */
inline constexpr std::uint64_t DEFAULT_SIZE_LIMIT = 10ULL * 1024ULL * 1024ULL;

/** @brief Average rate of the simulated clock when `--rate` is omitted. */
inline constexpr double DEFAULT_SIMULATED_RATE = 20.0;

/** @brief Conditions that end a run; generation stops at the first one reached. */
struct Limits
{
    std::optional<std::uint64_t> lines;
    std::optional<std::uint64_t> bytes;
    /** Covered time span: wall-clock time when live, virtual time when simulated. */
    std::optional<std::chrono::milliseconds> duration;
    /** Runs until interrupted; no other limit is set. */
    bool forever = false;
};

/** @brief How record timestamps advance and how fast lines are written. */
struct TimingOptions
{
    /** Start of the virtual clock; when set, the run is simulated and never sleeps. */
    std::optional<std::chrono::system_clock::time_point> startTime;
    /** Average lines per second. When unset, a live run is unthrottled. */
    std::optional<double> rate;
    test_common::TrafficProfile profile = test_common::TrafficProfile::Daily;
    bool bursts = false;

    /** @brief Returns whether timestamps come from a virtual clock. */
    bool Simulated() const noexcept
    {
        return startTime.has_value();
    }
};

/** @brief Fully validated settings for one generator run. */
struct GeneratorOptions
{
    const FormatSpec *format = FindFormat("json");
    Target target;
    bool append = false;
    bool quiet = false;
    /** RNG seed; a random seed is chosen when unset. */
    std::optional<std::uint32_t> seed;
    Limits limits;
    TimingOptions timing;
    RotationOptions rotation;
    /** TLS settings, used only by `TargetKind::TcpTls`. */
    test_common::TcpLogClient::TlsOptions tls;
};

/**
 * @brief Registers every command-line option on @p app.
 *
 * Parsed values are written into @p options. Cross-option checks run in the
 * app's final callback, which also derives the default output file name and
 * the default size limit, so a successful `CLI::App::parse` leaves
 * @p options ready for `Run()`. `--list-formats` prints the format table and
 * ends parsing with `CLI::Success`.
 *
 * @param app Application to configure.
 * @param options Destination for parsed values; must outlive parsing.
 */
void AddOptions(CLI::App &app, GeneratorOptions &options);

/**
 * @brief Parses a `--start-time` value.
 *
 * @param text `now`, or a UTC time `YYYY-MM-DD[THH:MM:SS[.mmm]]` with an optional trailing `Z`.
 * @return The parsed time.
 * @throws std::invalid_argument when @p text has another form.
 */
std::chrono::system_clock::time_point ParseStartTime(const std::string &text);

} // namespace log_generator
