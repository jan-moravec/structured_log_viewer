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

/** @brief Frees a compiled PCRE2 pattern, so it can be held in a unique_ptr. */
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

/** @brief Frees PCRE2 match data, so it can be held in a unique_ptr. */
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

/** @brief Frees a PCRE2 match context, so it can be held in a unique_ptr. */
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

/** @brief Describes a PCRE2 error code in a short message. */
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
 * @brief Compiles a pattern with the given PCRE2 options, for example PCRE2_UTF.
 *
 * Returns null if the pattern is invalid. Then errcode holds the PCRE2 error code and erroffset the position of the
 * error in the pattern.
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
