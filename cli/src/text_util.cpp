#include "text_util.hpp"

#include <algorithm>
#include <cctype>

namespace slp
{

namespace
{

[[nodiscard]] char LowerChar(char ch) noexcept
{
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

[[nodiscard]] bool IsSpace(char ch) noexcept
{
    return std::isspace(static_cast<unsigned char>(ch)) != 0;
}

} // namespace

std::string AsciiLower(std::string_view text)
{
    std::string out(text);
    std::ranges::transform(out, out.begin(), LowerChar);
    return out;
}

bool EqualsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept
{
    return std::ranges::equal(lhs, rhs, [](char a, char b) { return LowerChar(a) == LowerChar(b); });
}

bool StartsWithIgnoreCase(std::string_view text, std::string_view prefix) noexcept
{
    return text.size() >= prefix.size() && EqualsIgnoreCase(text.substr(0, prefix.size()), prefix);
}

std::string JoinComma(const std::vector<std::string> &parts)
{
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        if (i != 0)
        {
            out += ", ";
        }
        out += parts[i];
    }
    return out;
}

std::vector<std::string> SplitCsv(std::string_view text)
{
    std::vector<std::string> parts;
    std::size_t begin = 0;
    while (begin <= text.size())
    {
        const std::size_t comma = text.find(',', begin);
        std::string_view part =
            comma == std::string_view::npos ? text.substr(begin) : text.substr(begin, comma - begin);
        while (!part.empty() && IsSpace(part.front()))
        {
            part.remove_prefix(1);
        }
        while (!part.empty() && IsSpace(part.back()))
        {
            part.remove_suffix(1);
        }
        if (!part.empty())
        {
            parts.emplace_back(part);
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        begin = comma + 1;
    }
    return parts;
}

} // namespace slp
