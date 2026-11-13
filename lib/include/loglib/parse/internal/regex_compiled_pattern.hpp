/**
 * @brief Compiled PCRE2 pattern shared by the regex parser and template detection.
 *
 * A CompiledPattern owns the compiled code, the match limits and the list of named groups in pattern order. It is
 * compiled once and shared read-only by the worker threads, each of which brings its own match data. Only library
 * sources include this header, because it pulls in the PCRE2 C API.
 *
 * This is part of the parse module.
 */
#pragma once

#include "loglib/core/internal/pcre2_support.hpp"

#include <fmt/format.h>

#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

namespace loglib::internal
{

/**
 * @brief The UTF-8 byte order mark. Some editors put it at the start of a text file. With the mark in place, anchors
 * such as ^date in the built-in templates cannot match at the first byte, and detection would refuse the file. It is
 * removed from the first probe line only.
 */
inline constexpr std::string_view UTF8_BOM = "\xEF\xBB\xBF";

/**
 * @brief The limits on PCRE2 backtracking that apply to every match.
 *
 * The defaults of 10 million are too generous for patterns from users. A pattern such as .*.*.* on a long line could
 * stall a parse for seconds. These limits leave well-behaved patterns alone and make a pathological line fail with a
 * parse error.
 */
inline constexpr uint32_t PCRE2_MATCH_LIMIT = 100'000;
inline constexpr uint32_t PCRE2_DEPTH_LIMIT = 1'000;

/**
 * @brief The named groups of a compiled pattern, in the order they appear in the pattern.
 *
 * PCRE2 lists the groups alphabetically, so they are sorted by group index. The key id at position i is the id of
 * groupNames[i], and a worker finds that group in the match vector at groupIndices[i] times 2. The order is visible to
 * users: columns follow the order in which key ids were created, so the columns of a pattern appear left to right as
 * written, for example clientip, ident, auth, timestamp for the common log format, and not alphabetically.
 */
struct PatternSchema
{
    std::vector<std::string> groupNames;
    std::vector<uint32_t> groupIndices;
};

/**
 * @brief A compiled PCRE2 pattern with its match limits and group schema. It is built once and shared, read-only, by
 * all worker threads.
 */
class CompiledPattern
{
public:
    CompiledPattern() = default;

    /**
     * @brief Compiles the pattern. Returns true on success. Otherwise errorOut gets a message for the parse error
     * report.
     *
     * A compiled pattern is ready to use. It is JIT-compiled where the platform allows and otherwise uses the
     * interpreter, and it has a match context with the match and depth limits.
     */
    bool Compile(std::string_view pattern, std::string &errorOut)
    {
        mPatternString.assign(pattern);

        int errcode = 0;
        PCRE2_SIZE erroffset = 0;
        mCode = internal::CompilePcre2(pattern, /*options*/ 0, errcode, erroffset);
        if (mCode == nullptr)
        {
            errorOut = fmt::format("Pattern compile failed at offset {}: {}", erroffset, FormatPcre2Error(errcode));
            return false;
        }

        // JIT is a hot-path optimisation; failure here is
        // non-fatal — matching falls back to the interpreted engine
        // (correct but slower). Recorded so unit tests can assert
        // JIT was actually used in CI.
        const int jitRc = pcre2_jit_compile(mCode.get(), PCRE2_JIT_COMPLETE);
        mJitCompiled = (jitRc == 0);

        // A pattern can switch on UTF mode itself, with a leading (*UTF).
        uint32_t allOptions = 0;
        mUtf =
            pcre2_pattern_info(mCode.get(), PCRE2_INFO_ALLOPTIONS, &allOptions) == 0 && (allOptions & PCRE2_UTF) != 0;

        // Match context shared read-only across workers; carries
        // the match/depth limits so a malicious pattern can't burn
        // CPU for seconds on a single line.
        pcre2_match_context *ctxRaw = pcre2_match_context_create(/*gcontext*/ nullptr);
        if (ctxRaw == nullptr)
        {
            errorOut = "Failed to allocate PCRE2 match context.";
            mCode.reset();
            return false;
        }
        mContext.reset(ctxRaw);
        pcre2_set_match_limit(mContext.get(), PCRE2_MATCH_LIMIT);
        pcre2_set_depth_limit(mContext.get(), PCRE2_DEPTH_LIMIT);

        ExtractSchema();
        return true;
    }

    [[nodiscard]] bool IsReady() const noexcept
    {
        return mCode != nullptr;
    }

    [[nodiscard]] bool HasNamedGroups() const noexcept
    {
        return !mSchema.groupNames.empty();
    }

    [[nodiscard]] const pcre2_code *Code() const noexcept
    {
        return mCode.get();
    }
    [[nodiscard]] pcre2_code *Code() noexcept
    {
        return mCode.get();
    }

    /**
     * @brief The match context. It is set up once by Compile and does not change afterwards.
     *
     * pcre2_match needs a non-const context, so this const function returns a non-const pointer. That avoids casts in
     * the parser.
     */
    [[nodiscard]] pcre2_match_context *Context() const noexcept
    {
        return mContext.get();
    }

    [[nodiscard]] const PatternSchema &Schema() const noexcept
    {
        return mSchema;
    }

    [[nodiscard]] const std::string &PatternString() const noexcept
    {
        return mPatternString;
    }

    [[nodiscard]] bool JitCompiled() const noexcept
    {
        return mJitCompiled;
    }

    /**
     * @brief True when the pattern runs in UTF mode, which it can turn on itself with a leading (*UTF).
     *
     * A UTF pattern must only see valid UTF-8, so its subjects go through the checked pcre2_match. Other patterns can
     * skip the check.
     */
    [[nodiscard]] bool Utf() const noexcept
    {
        return mUtf;
    }

    /**
     * @brief Creates match data of the right size for this pattern. Each worker needs its own. The caller owns the
     * result.
     */
    [[nodiscard]] Pcre2MatchDataPtr NewMatchData() const
    {
        if (mCode == nullptr)
        {
            return {};
        }
        return Pcre2MatchDataPtr(pcre2_match_data_create_from_pattern(mCode.get(), nullptr));
    }

private:
    void ExtractSchema()
    {
        mSchema.groupNames.clear();
        mSchema.groupIndices.clear();

        uint32_t nameCount = 0;
        pcre2_pattern_info(mCode.get(), PCRE2_INFO_NAMECOUNT, &nameCount);
        if (nameCount == 0)
        {
            return;
        }
        uint32_t entrySize = 0;
        pcre2_pattern_info(mCode.get(), PCRE2_INFO_NAMEENTRYSIZE, &entrySize);
        PCRE2_SPTR table = nullptr;
        // pcre2_pattern_info takes a void * out-parameter; going
        // straight from PCRE2_SPTR * (a two-level pointer) is a
        // multi-level implicit conversion. Route via void * so
        // clang-tidy's bugprone-multi-level-implicit-pointer-conversion
        // stays quiet without changing the ABI-level call.
        pcre2_pattern_info(mCode.get(), PCRE2_INFO_NAMETABLE, static_cast<void *>(&table));
        if (table == nullptr || entrySize < 3)
        {
            return;
        }

        mSchema.groupNames.reserve(nameCount);
        mSchema.groupIndices.reserve(nameCount);
        for (uint32_t i = 0; i < nameCount; ++i)
        {
            const auto *entry = table + (i * entrySize);
            // First two bytes: group index (big-endian uint16).
            const uint32_t groupIndex = (static_cast<uint32_t>(entry[0]) << 8U) | static_cast<uint32_t>(entry[1]);
            // Remaining bytes: NUL-terminated UTF-8 name.
            const auto *name = reinterpret_cast<const char *>(entry + 2);
            mSchema.groupNames.emplace_back(name);
            mSchema.groupIndices.push_back(groupIndex);
        }

        // Re-sort into pattern-source order. PCRE2 gives us the
        // name table sorted alphabetically by name; sorting by
        // group index here is what makes columns follow the
        // order the user wrote (see PatternSchema
        // doc). groupNames and groupIndices are parallel
        // arrays, so we permute them together via an index vector
        // — cheap, once at compile time.
        std::vector<size_t> order(mSchema.groupIndices.size());
        std::iota(order.begin(), order.end(), 0U);
        std::sort(order.begin(), order.end(), [this](size_t lhs, size_t rhs) {
            return mSchema.groupIndices[lhs] < mSchema.groupIndices[rhs];
        });
        std::vector<std::string> sortedNames;
        std::vector<uint32_t> sortedIndices;
        sortedNames.reserve(order.size());
        sortedIndices.reserve(order.size());
        for (const size_t i : order)
        {
            sortedNames.emplace_back(std::move(mSchema.groupNames[i]));
            sortedIndices.push_back(mSchema.groupIndices[i]);
        }
        mSchema.groupNames = std::move(sortedNames);
        mSchema.groupIndices = std::move(sortedIndices);
    }

    Pcre2CodePtr mCode;
    Pcre2MatchContextPtr mContext;
    PatternSchema mSchema;
    std::string mPatternString;
    bool mJitCompiled = false;
    bool mUtf = false;
};

} // namespace loglib::internal
