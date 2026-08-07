#include "loglib/log_parser.hpp"

#include <cstddef>
#include <fstream>
#include <string>
#include <vector>

namespace loglib
{

bool LogParser::IsValid(const std::filesystem::path &file) const
{
    const std::string head = ReadProbeHead(file, PROBE_BYTES_BUDGET);
    if (head.empty())
    {
        return false;
    }
    return IsValidBytes(std::string_view(head));
}

std::string ReadProbeHead(const std::filesystem::path &file, std::size_t budget)
{
    std::ifstream stream(file, std::ios::binary);
    if (!stream.is_open())
    {
        return {};
    }

    std::string head;
    head.resize(budget);
    stream.read(head.data(), static_cast<std::streamsize>(budget));
    const auto got = stream.gcount();
    if (got <= 0)
    {
        return {};
    }
    head.resize(static_cast<std::size_t>(got));
    return head;
}

} // namespace loglib
