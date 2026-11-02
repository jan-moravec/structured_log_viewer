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

/// UTF-8 BOM. Some editors (Notepad, older PowerShell) prepend it
/// to text files; with the BOM intact the `^date` / `^IP` / `^[`
/// anchors in the built-in templates can't bind to position 0 and
/// auto-detect silently refuses every file. Stripped from the first
/// probe line only, leaving the rest of the byte stream untouched.
inline constexpr std::string_view UTF8_BOM = "\xEF\xBB\xBF";

/// PCRE2 backtracking limits applied to every match. The defaults
/// (10M / 10M) are too permissive for untrusted patterns —
/// something like `.*.*.*` against a long line could otherwise
/// stall the parse for seconds. These values leave well-behaved
/// patterns (lnav / grok) untouched and surface a `ParsedLineError`
/// on pathological inputs.
inline constexpr uint32_t PCRE2_MATCH_LIMIT = 100'000;
inline constexpr uint32_t PCRE2_DEPTH_LIMIT = 1'000;

// ---------------------------------------------------------------------
// CompiledPattern: shared compiled state for one PCRE2 pattern.

/// Schema of a compiled pattern: every named capture group, sorted
/// by `groupIndex` so the schema reads in pattern-source order
/// (PCRE2 returns them alphabetically via
/// `pcre2_pattern_info(PCRE2_INFO_NAMETABLE, ...)`). The KeyId at
/// `[i]` is `keys.GetOrInsert(groupNames[i])`; workers index into
/// the ovector at `groupIndices[i] * 2`. Source order is
/// consumer-visible: column order in `LogTable` follows KeyId
/// allocation, so columns appear left-to-right as the user wrote
/// them (e.g. CLF: `clientip ident auth timestamp verb ...`, not
/// alphabetical `auth bytes clientip ...`).
struct PatternSchema
{
    std::vector<std::string> groupNames;
    std::vector<uint32_t> groupIndices;
};

class CompiledPattern
{
public:
    CompiledPattern() = default;

    /// Returns true on success. On failure, populates @p errorOut
    /// with a human-readable message (used in parse-time error
    /// reporting). On success the pattern is fully ready:
    /// JIT-compiled where the platform allows (silently falls back
    /// to the interpreted matcher otherwise), with a match context
    /// carrying the project's match/depth limits.
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

    /// The match context is set up once during `Compile()` (its
    /// limits never change afterwards) and PCRE2's `pcre2_match`
    /// requires a non-const context. Exposing a single method
    /// returning `pcre2_match_context *` even from a logically
    /// const `CompiledPattern` avoids sprinkling `const_cast`
    /// throughout the parser; `unique_ptr::get() const` already
    /// returns the mutable pointer type.
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

    /// Allocate per-worker match data sized to this pattern's
    /// capture count. PCRE2 documents
    /// `pcre2_match_data_create_from_pattern` as the canonical
    /// sizing call. Caller owns the returned handle.
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
        // `pcre2_pattern_info` takes a `void *` out-parameter; going
        // straight from `PCRE2_SPTR *` (a two-level pointer) is a
        // multi-level implicit conversion. Route via `void *` so
        // clang-tidy's `bugprone-multi-level-implicit-pointer-conversion`
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
        // order the user wrote (see `PatternSchema`
        // doc). `groupNames` and `groupIndices` are parallel
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
};

} // namespace loglib::internal
