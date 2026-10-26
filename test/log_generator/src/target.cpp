#include <log_generator/target.hpp>

#include <charconv>
#include <stdexcept>
#include <system_error>

namespace log_generator
{

std::string Target::Describe() const
{
    const auto hostPort = [this]() {
        const bool ipv6 = host.contains(':');
        return (ipv6 ? "[" + host + "]" : host) + ":" + std::to_string(port);
    };
    switch (kind)
    {
    case TargetKind::File:
        return path.string();
    case TargetKind::Stdout:
        return "stdout";
    case TargetKind::Tcp:
        return "tcp://" + hostPort();
    case TargetKind::TcpTls:
        return "tcp+tls://" + hostPort();
    case TargetKind::Udp:
        return "udp://" + hostPort();
    }
    return {};
}

Target ParseTarget(std::string_view text)
{
    if (text.empty())
    {
        throw std::invalid_argument("output must not be empty");
    }
    if (text == "-")
    {
        return Target{.kind = TargetKind::Stdout};
    }

    const auto schemeAt = text.find("://");
    if (schemeAt == std::string_view::npos)
    {
        return Target{.kind = TargetKind::File, .path = std::filesystem::path(text)};
    }

    const std::string full(text);
    const std::string_view scheme = text.substr(0, schemeAt);
    const std::string_view hostPort = text.substr(schemeAt + 3);
    Target target;
    if (scheme == "tcp")
    {
        target.kind = TargetKind::Tcp;
    }
    else if (scheme == "tcp+tls")
    {
        target.kind = TargetKind::TcpTls;
    }
    else if (scheme == "udp")
    {
        target.kind = TargetKind::Udp;
    }
    else
    {
        throw std::invalid_argument(
            "unknown scheme in '" + full + "' (expected tcp://, tcp+tls://, or udp://; use '-' for stdout)"
        );
    }

    std::string_view hostText;
    std::string_view portText;
    if (hostPort.starts_with('['))
    {
        const auto closeBracket = hostPort.find(']');
        if (closeBracket == std::string_view::npos || closeBracket + 1 >= hostPort.size() ||
            hostPort[closeBracket + 1] != ':')
        {
            throw std::invalid_argument("'" + full + "' must be of the form scheme://[IPV6]:PORT");
        }
        hostText = hostPort.substr(1, closeBracket - 1);
        portText = hostPort.substr(closeBracket + 2);
    }
    else
    {
        const auto colon = hostPort.rfind(':');
        if (colon == std::string_view::npos)
        {
            throw std::invalid_argument("'" + full + "' must be of the form scheme://HOST:PORT");
        }
        hostText = hostPort.substr(0, colon);
        portText = hostPort.substr(colon + 1);
    }
    if (hostText.empty() || portText.empty())
    {
        throw std::invalid_argument("'" + full + "' must be of the form scheme://HOST:PORT");
    }

    unsigned port = 0;
    const auto [end, ec] = std::from_chars(portText.data(), portText.data() + portText.size(), port);
    if (ec != std::errc{} || end != portText.data() + portText.size() || port == 0 || port > 65535)
    {
        throw std::invalid_argument("'" + full + "' must have a port in 1..65535");
    }
    target.host = std::string(hostText);
    target.port = static_cast<std::uint16_t>(port);
    return target;
}

} // namespace log_generator
