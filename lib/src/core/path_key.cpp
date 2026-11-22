#include "loglib/core/path_key.hpp"

#include <cctype>
#include <string>
#include <system_error>
#include <utility>

namespace loglib
{

namespace
{

#ifdef _WIN32
/** Lowercases ASCII bytes without altering multibyte UTF-8 sequences. This is not Unicode case folding. */
std::string FoldCase(std::string text)
{
    constexpr unsigned char ASCII_BOUND = 0x80;
    for (char &c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < ASCII_BOUND)
        {
            c = static_cast<char>(std::tolower(byte));
        }
    }
    return text;
}
#endif

} // namespace

std::string MakePathKey(const std::filesystem::path &path)
{
    if (path.empty())
    {
        return {};
    }
    // Normalize by text. Resolving links or requiring the target to exist would make a key for a moved file differ from
    // the key saved earlier.
    std::error_code ec;
    std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    if (ec || absolute.empty())
    {
        absolute = path;
    }
    std::filesystem::path normalized = absolute.lexically_normal();
    if (normalized.empty())
    {
        normalized = path;
    }
    // The generic form has forward slashes, and UTF-8 keeps non-ASCII names intact.
    const std::u8string utf8 = normalized.generic_u8string();
    std::string key(reinterpret_cast<const char *>(utf8.data()), utf8.size());
#ifdef _WIN32
    return FoldCase(std::move(key));
#else
    return key;
#endif
}

} // namespace loglib
