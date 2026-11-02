/**
 * @brief Small PCRE2 helpers shared by the regex parser and the string matcher.
 *
 * Owning pointers for the PCRE2 handles, a function that turns an error code into text, and a function that compiles a
 * pattern. Keeping them in one place means both users free handles and report errors the same way. Only library
 * sources include this header, because it pulls in the PCRE2 C API.
 *
 * This is part of the core module.
 */
#pragma once

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <fmt/format.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace loglib::internal
{

struct Pcre2CodeDeleter
{
    void operator()(pcre2_code *code) const noexcept
    {
        if (code != nullptr)
        {
            pcre2_code_free(code);
        }
    }
};
using Pcre2CodePtr = std::unique_ptr<pcre2_code, Pcre2CodeDeleter>;

struct Pcre2MatchDataDeleter
{
    void operator()(pcre2_match_data *matchData) const noexcept
    {
        if (matchData != nullptr)
        {
            pcre2_match_data_free(matchData);
        }
    }
};
using Pcre2MatchDataPtr = std::unique_ptr<pcre2_match_data, Pcre2MatchDataDeleter>;

struct Pcre2MatchContextDeleter
{
    void operator()(pcre2_match_context *context) const noexcept
    {
        if (context != nullptr)
        {
            pcre2_match_context_free(context);
        }
    }
};
using Pcre2MatchContextPtr = std::unique_ptr<pcre2_match_context, Pcre2MatchContextDeleter>;

/**
 * @brief Describes a PCRE2 error code in a short message.
 *
 * @param errcode Code returned by a PCRE2 call.
 */
[[nodiscard]] inline std::string FormatPcre2Error(int errcode)
{
    constexpr std::size_t ERROR_BUFFER_SIZE = 256;
    PCRE2_UCHAR8 buffer[ERROR_BUFFER_SIZE] = {};
    const int length = pcre2_get_error_message(errcode, buffer, sizeof(buffer));
    if (length <= 0)
    {
        return fmt::format("PCRE2 error {}", errcode);
    }
    return {reinterpret_cast<const char *>(buffer), static_cast<std::size_t>(length)};
}

/**
 * @brief Compiles @p pattern.
 *
 * @param pattern Pattern text.
 * @param options PCRE2 compile options, for example PCRE2_UTF.
 * @param errcode Receives the PCRE2 error code when compiling fails.
 * @param erroffset Receives the offset of the error in the pattern when compiling fails.
 * @return The compiled code, or null when the pattern is invalid.
 */
[[nodiscard]] inline Pcre2CodePtr CompilePcre2(
    std::string_view pattern, std::uint32_t options, int &errcode, PCRE2_SIZE &erroffset
)
{
    return Pcre2CodePtr(pcre2_compile(
        reinterpret_cast<PCRE2_SPTR>(pattern.data()),
        static_cast<PCRE2_SIZE>(pattern.size()),
        options,
        &errcode,
        &erroffset,
        /*ccontext*/ nullptr
    ));
}

} // namespace loglib::internal
