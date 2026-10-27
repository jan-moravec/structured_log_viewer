#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace log_generator
{

/** @brief Kind of destination generated lines are written to. */
enum class TargetKind
{
    File,
    Stdout,
    Tcp,
    TcpTls,
    Udp,
};

/** @brief Destination parsed from `--output`. */
struct Target
{
    TargetKind kind = TargetKind::File;
    /** Output file; used by `TargetKind::File`. */
    std::filesystem::path path;
    /** Peer host; used by the network kinds. */
    std::string host;
    /** Peer port; used by the network kinds. */
    std::uint16_t port = 0;

    /** @brief Returns whether lines go to a TCP, TLS, or UDP peer. */
    bool IsNetwork() const noexcept
    {
        return kind == TargetKind::Tcp || kind == TargetKind::TcpTls || kind == TargetKind::Udp;
    }

    /** @brief Returns a short human-readable form, such as `udp://127.0.0.1:5142`. */
    std::string Describe() const;
};

/**
 * @brief Parses an `--output` value.
 *
 * `-` selects standard output. `tcp://HOST:PORT`, `tcp+tls://HOST:PORT`, and
 * `udp://HOST:PORT` select a network peer; IPv6 hosts use brackets, as in
 * `tcp://[::1]:5141`. Any other value without `://` is a file path.
 *
 * @param text Value to parse.
 * @return The parsed target.
 * @throws std::invalid_argument when @p text is empty, has an unknown scheme,
 *         or has a malformed host or port.
 */
Target ParseTarget(std::string_view text);

} // namespace log_generator
