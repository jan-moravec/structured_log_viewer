#include "loglib/parsers/regex_parser.hpp"

#include "loglib/file_line_source.hpp"
#include "loglib/internal/advanced_parser_options.hpp"
#include "loglib/internal/classify_bare_scalar.hpp"
#include "loglib/internal/compact_log_value.hpp"
#include "loglib/internal/line_decoder.hpp"
#include "loglib/internal/static_parser_pipeline.hpp"
#include "loglib/internal/streaming_parse_loop.hpp"
#include "loglib/log_file.hpp"
#include "loglib/log_line.hpp"
#include "loglib/log_processing.hpp"
#include "loglib/regex_templates.hpp"
#include "loglib/stream_line_source.hpp"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include <fmt/format.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

/// Bytes scanned by `IsValid` / `DetectRegexTemplate` before giving
/// up. Matches CSV's probe budget.
constexpr size_t IS_VALID_PROBE_BYTES = 16 * 1024;

/// Minimum number of non-blank probe lines a template must match for
/// auto-detection to claim the file. Two is enough to rule out the
/// "one ad-hoc line that happens to look like syslog" case without
/// disqualifying short files.
constexpr size_t IS_VALID_MIN_MATCHES = 2;

/// PCRE2 backtracking limits applied to every match. Defaults (10M /
/// 10M) are far too permissive for untrusted patterns — a user-typed
/// `.*.*.*` against a long line could otherwise stall the parse for
/// seconds. These values let well-behaved patterns (lnav / grok) run
/// untouched and surface a `ParsedLineError` for pathological inputs.
constexpr uint32_t PCRE2_MATCH_LIMIT = 100'000;
constexpr uint32_t PCRE2_DEPTH_LIMIT = 1'000;

// ---------------------------------------------------------------------
// RAII wrappers around the PCRE2 C handles.
// ---------------------------------------------------------------------

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
    void operator()(pcre2_match_data *md) const noexcept
    {
        if (md != nullptr)
        {
            pcre2_match_data_free(md);
        }
    }
};
using Pcre2MatchDataPtr = std::unique_ptr<pcre2_match_data, Pcre2MatchDataDeleter>;

struct Pcre2MatchContextDeleter
{
    void operator()(pcre2_match_context *ctx) const noexcept
    {
        if (ctx != nullptr)
        {
            pcre2_match_context_free(ctx);
        }
    }
};
using Pcre2MatchContextPtr = std::unique_ptr<pcre2_match_context, Pcre2MatchContextDeleter>;

/// Decode a PCRE2 errcode to a short message for parse-error
/// reporting. Buffer sized per upstream advice (`pcre2demo.c`).
std::string FormatPcre2Error(int errcode)
{
    PCRE2_UCHAR8 buffer[256] = {};
    const int len = pcre2_get_error_message(errcode, buffer, sizeof(buffer));
    if (len <= 0)
    {
        return fmt::format("PCRE2 error {}", errcode);
    }
    return {reinterpret_cast<const char *>(buffer), static_cast<size_t>(len)};
}

// ---------------------------------------------------------------------
// CompiledPattern: shared compiled state for one PCRE2 pattern.
// ---------------------------------------------------------------------

/// Schema of a compiled pattern: every named capture group, sorted
/// by `groupIndex` so the schema reads in pattern-source order
/// rather than the alphabetical order PCRE2 returns from
/// `pcre2_pattern_info(PCRE2_INFO_NAMETABLE, ...)`. The KeyId at
/// `[i]` is `keys.GetOrInsert(groupNames[i])`; the worker indexes
/// into the ovector at `groupIndices[i] * 2`. Pattern-source order
/// matters for two consumer-visible properties:
///  - column order in `LogTable` follows KeyId allocation order,
///    so columns appear left-to-right the way the user wrote them
///    (e.g. CLF: `clientip ident auth timestamp verb ...`, not
///    alphabetical `auth bytes clientip ...`).
///  - `RegexParser::ToString` joins values in pattern-source order
///    so a regenerated line at least has its fields in the same
///    slots the original line did.
struct PatternSchema
{
    std::vector<std::string> groupNames;
    std::vector<uint32_t> groupIndices;
};

class CompiledPattern
{
public:
    CompiledPattern() = default;

    /// Returns true on success; @p errorOut is populated on failure
    /// with a human-readable message (used in parse-time error
    /// reporting). On success the pattern is fully ready: JIT-compiled
    /// where the platform supports it (silently falls back to the
    /// interpreted matcher otherwise), with a match context that
    /// carries the project's match/depth limits.
    bool Compile(std::string_view pattern, std::string &errorOut)
    {
        mPatternString.assign(pattern);

        int errcode = 0;
        PCRE2_SIZE erroffset = 0;
        pcre2_code *raw = pcre2_compile(
            reinterpret_cast<PCRE2_SPTR>(pattern.data()),
            pattern.size(),
            /*options*/ 0,
            &errcode,
            &erroffset,
            /*ccontext*/ nullptr
        );
        if (raw == nullptr)
        {
            errorOut = fmt::format("Pattern compile failed at offset {}: {}", erroffset, FormatPcre2Error(errcode));
            return false;
        }
        mCode.reset(raw);

        // JIT is a hot-path optimisation. A failure here is non-fatal:
        // matching falls back to the interpreted engine, which is
        // correct just slower. Recorded so unit tests can assert JIT
        // was actually used in CI.
        const int jitRc = pcre2_jit_compile(mCode.get(), PCRE2_JIT_COMPLETE);
        mJitCompiled = (jitRc == 0);

        // Match context shared read-only across workers; carries the
        // match/depth limits so a malicious pattern can't burn CPU
        // for seconds on a single line.
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

    [[nodiscard]] const pcre2_match_context *Context() const noexcept
    {
        return mContext.get();
    }
    [[nodiscard]] pcre2_match_context *Context() noexcept
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

    /// Allocate per-worker match data sized to this pattern's capture
    /// count. PCRE2 documents `pcre2_match_data_create_from_pattern`
    /// as the canonical sizing call. Ownership returned to the caller.
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
        pcre2_pattern_info(mCode.get(), PCRE2_INFO_NAMETABLE, &table);
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

        // Re-sort into pattern-source order. PCRE2 hands us the name
        // table sorted alphabetically by name; sorting by group index
        // here is what makes columns and `ToString` follow the order
        // the user wrote in the pattern (see `PatternSchema` doc).
        // `groupNames` and `groupIndices` are parallel arrays so we
        // permute them together via an index vector — cheap, runs
        // once at compile time.
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

// ---------------------------------------------------------------------
// Built-in template compile cache (used by IsValid / DetectRegexTemplate).
// ---------------------------------------------------------------------

struct CompiledTemplate
{
    const RegexTemplate *source = nullptr;
    CompiledPattern compiled;
};

/// Lazy, thread-safe singleton. Built once at first IsValid /
/// DetectRegexTemplate call; the compile cost (one
/// pcre2_compile + pcre2_jit_compile per template) is amortised
/// across every subsequent file probe.
const std::vector<CompiledTemplate> &CompiledBuiltins()
{
    static const std::vector<CompiledTemplate> CACHE = [] {
        std::vector<CompiledTemplate> v;
        const auto builtins = BuiltinRegexTemplates();
        v.reserve(builtins.size());
        for (const RegexTemplate &tmpl : builtins)
        {
            CompiledTemplate c;
            c.source = &tmpl;
            std::string err;
            (void)c.compiled.Compile(tmpl.pattern, err);
            // A failed compile here is a programmer error in the
            // registry — drop the entry rather than crashing so
            // auto-detect keeps working for the rest of the formats.
            if (c.compiled.IsReady())
            {
                v.push_back(std::move(c));
            }
        }
        return v;
    }();
    return CACHE;
}

/// True iff @p code matches @p line. The built-in templates are all
/// explicitly anchored with `^...$` in `regex_templates.cpp`, so a
/// successful `pcre2_match` already means the pattern consumed the
/// whole line — we don't need to pass `PCRE2_ANCHORED |
/// PCRE2_ENDANCHORED` here. A per-call `pcre2_match_data` is used
/// because the cache is shared across the process; allocating one
/// tiny block is cheap next to the match itself.
bool BuiltinMatchesFully(const CompiledPattern &cp, std::string_view line)
{
    const Pcre2MatchDataPtr md = cp.NewMatchData();
    if (md == nullptr)
    {
        return false;
    }
    const int rc = pcre2_match(
        cp.Code(),
        reinterpret_cast<PCRE2_SPTR>(line.data()),
        line.size(),
        /*startoffset*/ 0,
        /*options*/ 0,
        md.get(),
        // `Context()` is logically const here (we only read limits);
        // PCRE2 requires the non-const overload.
        const_cast<pcre2_match_context *>(cp.Context())
    );
    return rc > 0;
}

/// UTF-8 BOM. Some editors (Notepad, older PowerShell) prepend it to
/// text files; with the BOM intact the `^date` / `^IP` / `^[` anchors
/// in the built-in templates can't bind to position 0 and auto-detect
/// silently refuses every file. Stripped from the first probe line
/// (only) to keep the rest of the byte stream untouched.
constexpr std::string_view UTF8_BOM = "\xEF\xBB\xBF";

/// File-level probe shared by `IsValid` and `DetectRegexTemplate`.
/// Returns the first builtin that matches at least
/// `IS_VALID_MIN_MATCHES` of the first ~16 KiB worth of non-blank
/// lines, or nullptr.
const RegexTemplate *ProbeBuiltinTemplates(const std::filesystem::path &file)
{
    std::ifstream stream(file);
    if (!stream.is_open())
    {
        return nullptr;
    }

    std::vector<std::string> probeLines;
    probeLines.reserve(8);
    std::string line;
    size_t bytesScanned = 0;
    bool firstLine = true;
    while (std::getline(stream, line))
    {
        if (firstLine && line.size() >= UTF8_BOM.size() && std::string_view(line).substr(0, UTF8_BOM.size()) == UTF8_BOM)
        {
            line.erase(0, UTF8_BOM.size());
        }
        firstLine = false;
        if (!line.empty() && line.back() == '\r')
        {
            line.pop_back();
        }
        bytesScanned += line.size() + 1;
        if (!line.empty())
        {
            probeLines.push_back(line);
        }
        if (bytesScanned >= IS_VALID_PROBE_BYTES || probeLines.size() >= 8)
        {
            break;
        }
    }

    if (probeLines.size() < IS_VALID_MIN_MATCHES)
    {
        // Pattern matching is brittle on a 1-line file; refuse rather
        // than declare a winner from a coin flip.
        return nullptr;
    }

    for (const CompiledTemplate &t : CompiledBuiltins())
    {
        size_t hits = 0;
        for (const std::string &l : probeLines)
        {
            if (BuiltinMatchesFully(t.compiled, l))
            {
                ++hits;
                if (hits >= IS_VALID_MIN_MATCHES)
                {
                    return t.source;
                }
            }
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------
// Schema interning: named groups -> KeyIds, in pcre2 name-table order.
// ---------------------------------------------------------------------

/// Pre-intern every named group as a `KeyId` so Stage B does not
/// touch the global `KeyIndex` per cell. Mirrors CSV's eager header
/// intern step.
std::vector<KeyId> InternSchemaKeys(const PatternSchema &schema, KeyIndex &keys)
{
    std::vector<KeyId> ids;
    ids.reserve(schema.groupNames.size());
    for (const std::string &name : schema.groupNames)
    {
        ids.push_back(keys.GetOrInsert(name));
    }
    return ids;
}

// ---------------------------------------------------------------------
// Match-and-emit: shared between the static and streaming pipelines.
// ---------------------------------------------------------------------

/// Run one `pcre2_match` against @p line and emit the captured named
/// groups as compact values. `out` is appended in source order
/// (caller sorts before constructing the `LogLine`). The decoder
/// reuses `ClassifyBareScalar` so numeric captures get typed (status
/// codes, byte counts, ...) — mirroring CSV's bare-cell typing.
///
/// `fileBegin`/`fileSize` enable the zero-copy `MmapSlice` fast path
/// for static-file parsing; streaming callers pass `nullptr`/0.
/// `errorOut` is populated on no-match / match-limit error and the
/// function returns false; the caller decides whether to surface it
/// as a `ParsedLineError` or a `LineDecodeResult::Error`.
bool MatchLineAndEmit(
    const CompiledPattern &compiled,
    pcre2_match_data *matchData,
    const std::vector<KeyId> &columnKeys,
    std::string_view line,
    const char *fileBegin,
    size_t fileSize,
    std::string &ownedArena,
    std::vector<std::pair<KeyId, internal::CompactLogValue>> &out,
    std::string &errorOut
)
{
    // Self-contained contract: callers used to have to clear `out`
    // and `errorOut` themselves, which made the function brittle if
    // a future caller forgot to reset on a previous error iteration.
    // Clearing here keeps the existing fast paths (the static and
    // streaming callers both already clear on their own hot loop,
    // so this is a no-op for them) without trusting the call site.
    out.clear();
    errorOut.clear();

    const int rc = pcre2_match(
        compiled.Code(),
        reinterpret_cast<PCRE2_SPTR>(line.data()),
        line.size(),
        /*startoffset*/ 0,
        /*options*/ 0,
        matchData,
        const_cast<pcre2_match_context *>(compiled.Context())
    );
    if (rc == PCRE2_ERROR_NOMATCH)
    {
        errorOut = "Line did not match the regex pattern.";
        return false;
    }
    if (rc == PCRE2_ERROR_MATCHLIMIT || rc == PCRE2_ERROR_DEPTHLIMIT)
    {
        errorOut = "Pattern match exceeded the backtracking limit on this line.";
        return false;
    }
    if (rc < 0)
    {
        errorOut = fmt::format("PCRE2 match error: {}", FormatPcre2Error(rc));
        return false;
    }

    const PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(matchData);
    const auto captureCount = static_cast<uint32_t>(rc);

    const auto &schema = compiled.Schema();
    out.reserve(schema.groupIndices.size());
    for (size_t i = 0; i < schema.groupIndices.size(); ++i)
    {
        const uint32_t groupIndex = schema.groupIndices[i];
        if (groupIndex >= captureCount)
        {
            continue;
        }
        const PCRE2_SIZE startOff = ovector[2 * groupIndex];
        const PCRE2_SIZE endOff = ovector[(2 * groupIndex) + 1];
        if (startOff == PCRE2_UNSET || endOff == PCRE2_UNSET || endOff < startOff)
        {
            // Optional groups (e.g. `(?<pid>\d+)?`) that did not
            // participate in the match are simply absent from the
            // row; lookups return monostate.
            continue;
        }
        const std::string_view captured(line.data() + startOff, endOff - startOff);
        if (captured.empty())
        {
            // Same convention as CSV: an empty capture is "field
            // present but blank" — drop it so it doesn't bloat the
            // per-line array.
            continue;
        }
        internal::CompactLogValue compact =
            internal::ClassifyBareScalar(captured, fileBegin, fileSize, ownedArena);
        out.emplace_back(columnKeys[i], compact);
    }
    return true;
}

// ---------------------------------------------------------------------
// Static-pipeline glue.
// ---------------------------------------------------------------------

struct RegexWorkerState
{
    Pcre2MatchDataPtr matchData;

    /// Lazily attach to @p compiled on first use. `enumerable_thread_specific`
    /// default-constructs us; we can't allocate at construction time because
    /// the worker doesn't know which compiled pattern to size against.
    void Ensure(const CompiledPattern &compiled)
    {
        if (!matchData)
        {
            matchData = compiled.NewMatchData();
        }
    }
};

/// Stage A token: a contiguous mmap range covering complete lines.
/// Identical to `CsvByteRange`; if regex grows a parser-specific
/// per-token field later it stays decoupled from CSV.
struct RegexByteRange
{
    uint64_t batchIndex = 0;
    const char *bytesBegin = nullptr;
    const char *bytesEnd = nullptr;
    const char *fileEnd = nullptr;
};

std::string_view StripCr(std::string_view s) noexcept
{
    if (!s.empty() && s.back() == '\r')
    {
        s.remove_suffix(1);
    }
    return s;
}

/// Strip a leading UTF-8 BOM from @p sv if present. Mirrors CSV's
/// helper; only valid at the start of the very first line of a file.
std::string_view StripBom(std::string_view sv) noexcept
{
    if (sv.starts_with(UTF8_BOM))
    {
        sv.remove_prefix(UTF8_BOM.size());
    }
    return sv;
}

void DecodeRegexBatch(
    const RegexByteRange &batch,
    internal::WorkerScratch<RegexWorkerState> &worker,
    KeyIndex &keys,
    FileLineSource &source,
    std::span<const internal::TimeColumnSpec> timeColumns,
    internal::ParsedPipelineBatch &parsed,
    const CompiledPattern &compiled,
    const std::vector<KeyId> &columnKeys
)
{
    (void)keys; // Schema is pre-interned in `columnKeys`.
    parsed.batchIndex = batch.batchIndex;

    worker.user.Ensure(compiled);

    const char *cursor = batch.bytesBegin;
    const char *end = batch.bytesEnd;
    const char *fileEnd = batch.fileEnd;
    const char *fileBegin = source.File().Data();
    const auto fileSize = static_cast<size_t>(fileEnd - fileBegin);

    const auto batchBytes = static_cast<size_t>(end - cursor);
    const size_t estimatedLines = (batchBytes / 64) + 1;
    parsed.lines.reserve(estimatedLines);
    parsed.localLineOffsets.reserve(estimatedLines);

    size_t relativeLineNumber = 1;
    std::vector<std::pair<KeyId, internal::CompactLogValue>> values;
    std::string lineError;

    while (cursor < end)
    {
        const char *lineStart = cursor;
        const char *newline = static_cast<const char *>(memchr(cursor, '\n', static_cast<size_t>(end - cursor)));
        const char *lineEnd = (newline != nullptr) ? newline : end;
        cursor = (newline != nullptr) ? newline + 1 : end;

        std::string_view line(lineStart, static_cast<size_t>(lineEnd - lineStart));
        line = StripCr(line);
        // Strip a leading UTF-8 BOM if and only if this line starts
        // at file byte 0 — keeps `^...` anchors in user / built-in
        // patterns binding after a BOM-prefixed editor save. Stage A
        // emits batches at line boundaries, so only the first line
        // of batch 0 can ever match this branch.
        if (lineStart == fileBegin)
        {
            line = StripBom(line);
        }

        // Pad by 1 on an unterminated last line so the final byte
        // isn't lost on `LogFile::GetLine` round-trips. Matches the
        // CSV pipeline.
        const uint64_t nextOffset = static_cast<uint64_t>(cursor - fileBegin) + (newline == nullptr ? 1U : 0U);
        parsed.localLineOffsets.push_back(nextOffset);

        if (line.empty())
        {
            ++relativeLineNumber;
            continue;
        }

        if (!MatchLineAndEmit(
                compiled, worker.user.matchData.get(), columnKeys, line, fileBegin, fileSize,
                parsed.ownedStringsArena, values, lineError
            ))
        {
            parsed.errors.push_back(internal::ParsedLineError{.relativeLine = relativeLineNumber, .body = lineError});
            lineError.clear();
            ++relativeLineNumber;
            continue;
        }

        // `LogLine` ctor asserts ascending KeyIds; name-table order
        // is alphabetical, not KeyId order.
        std::sort(values.begin(), values.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

        LogLine logLine(std::move(values), keys, source, relativeLineNumber - 1);
        parsed.lines.push_back(std::move(logLine));
        worker.PromoteTimestamps(parsed.lines.back(), timeColumns, std::string_view(parsed.ownedStringsArena));

        values.clear();
        ++relativeLineNumber;
    }

    parsed.totalLineCount = relativeLineNumber - 1;
}

// ---------------------------------------------------------------------
// Streaming-loop glue.
// ---------------------------------------------------------------------

class RegexLineDecoder
{
public:
    RegexLineDecoder(const CompiledPattern &compiled, const std::vector<KeyId> &columnKeys)
        : mCompiled(&compiled), mColumnKeys(&columnKeys)
    {
        mMatchData = compiled.NewMatchData();
    }

    internal::LineDecodeResult DecodeCompact(
        std::string_view line,
        KeyIndex &keys,
        internal::PerWorkerKeyCache *keyCache,
        std::vector<std::pair<KeyId, internal::CompactLogValue>> &out,
        std::string &outOwnedArena,
        std::string &errorOut
    )
    {
        (void)keys;     // Schema pre-interned.
        (void)keyCache; // ditto.
        out.clear();
        outOwnedArena.clear();
        // Strip a leading UTF-8 BOM from the very first line of the
        // stream. The flag latches once so a literal `\xEF\xBB\xBF`
        // appearing inside the body of a later line stays intact.
        if (!mSawFirstLine)
        {
            line = StripBom(line);
            mSawFirstLine = true;
        }
        if (line.empty())
        {
            // The streaming loop already filters blank lines, so this
            // branch is defensive; `Skip` (no row, no error) is the
            // semantically correct return if it is ever reached.
            return internal::LineDecodeResult::Skip;
        }
        if (!MatchLineAndEmit(
                *mCompiled, mMatchData.get(), *mColumnKeys, line,
                /*fileBegin=*/nullptr, /*fileSize=*/0, outOwnedArena, out, errorOut
            ))
        {
            return internal::LineDecodeResult::Error;
        }
        return internal::LineDecodeResult::Emit;
    }

private:
    const CompiledPattern *mCompiled;
    const std::vector<KeyId> *mColumnKeys;
    Pcre2MatchDataPtr mMatchData;
    bool mSawFirstLine = false;
};

static_assert(
    internal::CompactLineDecoder<RegexLineDecoder>, "RegexLineDecoder must satisfy the CompactLineDecoder concept"
);

// ---------------------------------------------------------------------
// Helpers to extract the active pattern.
// ---------------------------------------------------------------------

/// Resolve the pattern to use for this parse. `explicitPattern` (the
/// pinned-pattern constructor / advanced overload) wins; otherwise we
/// fall back to the configuration snapshot's `Source::regexPattern`.
std::string ResolvePattern(const std::optional<std::string> &explicitPattern, const ParserOptions &options)
{
    if (explicitPattern.has_value())
    {
        return *explicitPattern;
    }
    if (options.configuration && options.configuration->source)
    {
        return options.configuration->source->regexPattern;
    }
    return {};
}

/// Single-error terminal pass: honour the sink contract (one
/// `OnBatch` before `OnFinished`) when we can't even start a pipeline
/// — empty pattern, bad pattern, missing named groups. @p streaming
/// picks the flush thresholds that match the surrounding pipeline so
/// the trailing batch's coalescer settings line up with the rest of
/// the parse (the constants only affect threshold-driven mid-parse
/// flushes; for a single-batch error pass both presets behave
/// identically, but the symmetry keeps the call sites obviously
/// consistent).
void EmitErrorAndFinish(
    LogParseSink &sink, std::string_view message, std::optional<size_t> newKeyBaseline, bool streaming = false
)
{
    sink.OnStarted();
    KeyIndex &keys = sink.Keys();
    const size_t flushLines =
        streaming ? internal::STREAMING_BATCH_FLUSH_LINES : internal::STATIC_BATCH_FLUSH_LINES;
    const auto flushInterval =
        streaming ? internal::STREAMING_BATCH_FLUSH_INTERVAL : internal::STATIC_BATCH_FLUSH_INTERVAL;
    internal::BatchCoalescer coalescer(sink, keys, flushLines, flushInterval, newKeyBaseline);
    coalescer.Pending().errors.emplace_back(std::string(message));
    coalescer.Finish(1, /*wasCancelled=*/false);
}

} // namespace

// ---------------------------------------------------------------------
// RegexParser public surface.
// ---------------------------------------------------------------------

RegexParser::RegexParser(std::string pattern) : mExplicitPattern(std::move(pattern))
{
}

bool RegexParser::IsValid(const std::filesystem::path &file) const
{
    return ProbeBuiltinTemplates(file) != nullptr;
}

void RegexParser::ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    // Capture the `KeyIndex` cursor *before* any potential intern.
    // Forwarded to `EmitErrorAndFinish` on the error paths (so the
    // sink-side new-key bookkeeping starts at the right place even
    // on failure) and to `RunStreamingParseLoop` on success so the
    // pre-interned named-group columns still surface as `newKeys` on
    // the first emitted batch. Without this baseline,
    // `BatchCoalescer` would start counting from the post-intern
    // size and `LogTable::AppendBatch` would never create the
    // columns -- live tail and network streams would ingest rows
    // with no visible columns.
    const size_t newKeyBaseline = sink.Keys().Size();

    const std::string pattern = ResolvePattern(mExplicitPattern, options);
    if (pattern.empty())
    {
        EmitErrorAndFinish(sink, "Regex parser requires a non-empty pattern.", newKeyBaseline, /*streaming=*/true);
        return;
    }

    CompiledPattern compiled;
    std::string compileError;
    if (!compiled.Compile(pattern, compileError))
    {
        EmitErrorAndFinish(sink, compileError, newKeyBaseline, /*streaming=*/true);
        return;
    }
    if (!compiled.HasNamedGroups())
    {
        EmitErrorAndFinish(
            sink,
            "Regex pattern has no named capture groups; nothing to put in columns. Use `(?<Name>...)`.",
            newKeyBaseline,
            /*streaming=*/true
        );
        return;
    }

    const std::vector<KeyId> columnKeys = InternSchemaKeys(compiled.Schema(), sink.Keys());

    RegexLineDecoder decoder(compiled, columnKeys);
    internal::RunStreamingParseLoop(source, decoder, sink, options, newKeyBaseline);
}

void RegexParser::ParseStreaming(FileLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    // Forward `mExplicitPattern` as an `optional<string_view>` (not
    // `string_view`) so the advanced overload can distinguish "no
    // pattern pinned" (fall back to options) from "pinned to an
    // empty pattern" (fail closed). Collapsing both to an empty
    // `string_view` would let a parser explicitly constructed with
    // `""` silently pick up the configuration's `regexPattern`,
    // diverging from the `StreamLineSource` overload's behaviour.
    std::optional<std::string_view> explicitView;
    if (mExplicitPattern.has_value())
    {
        explicitView = std::string_view(*mExplicitPattern);
    }
    ParseStreaming(source, sink, options, internal::AdvancedParserOptions{}, explicitView);
}

void RegexParser::ParseStreaming(
    FileLineSource &source,
    LogParseSink &sink,
    const ParserOptions &options,
    internal::AdvancedParserOptions advanced,
    std::optional<std::string_view> explicitPattern
)
{
    const std::string pattern = explicitPattern.has_value()
                                    ? std::string(*explicitPattern)
                                    : ResolvePattern(/*explicitPattern=*/std::nullopt, options);

    const LogFile &file = source.File();
    const char *fileBegin = file.Data();
    const size_t fileSize = file.Size();
    const char *fileEnd = (fileBegin != nullptr) ? fileBegin + fileSize : nullptr;

    const size_t newKeyBaseline = sink.Keys().Size();

    if (pattern.empty())
    {
        EmitErrorAndFinish(sink, "Regex parser requires a non-empty pattern.", newKeyBaseline);
        return;
    }

    // Compile once on the orchestrator thread; the pipeline shares
    // the compiled code read-only across all Stage B workers.
    // `RunStaticParserPipeline` runs synchronously (it joins every
    // TBB worker before returning), so a stack value captured by
    // reference outlives every concurrent use — no shared_ptr or
    // heap allocation needed.
    CompiledPattern compiled;
    std::string compileError;
    if (!compiled.Compile(pattern, compileError))
    {
        EmitErrorAndFinish(sink, compileError, newKeyBaseline);
        return;
    }
    if (!compiled.HasNamedGroups())
    {
        EmitErrorAndFinish(
            sink,
            "Regex pattern has no named capture groups; nothing to put in columns. Use `(?<Name>...)`.",
            newKeyBaseline
        );
        return;
    }

    const std::vector<KeyId> columnKeys = InternSchemaKeys(compiled.Schema(), sink.Keys());

    const size_t batchSize = advanced.batchSizeBytes != 0 ? advanced.batchSizeBytes
                                                          : internal::AdvancedParserOptions::DEFAULT_BATCH_SIZE_BYTES;

    const char *cursor = fileBegin;
    uint64_t batchIndex = 0;
    auto stageA = [cursor, fileEnd, batchSize, batchIndex](RegexByteRange &out) mutable -> bool {
        if (cursor == nullptr || cursor >= fileEnd)
        {
            return false;
        }
        const char *batchBegin = cursor;
        const auto remaining = static_cast<size_t>(fileEnd - cursor);
        const size_t advance = std::min(batchSize, remaining);
        const char *target = cursor + advance;
        if (advance < remaining)
        {
            const char *newline =
                static_cast<const char *>(memchr(target, '\n', static_cast<size_t>(fileEnd - target)));
            cursor = (newline != nullptr) ? newline + 1 : fileEnd;
        }
        else
        {
            cursor = fileEnd;
        }
        out.batchIndex = batchIndex++;
        out.bytesBegin = batchBegin;
        out.bytesEnd = cursor;
        out.fileEnd = fileEnd;
        return true;
    };

    FileLineSource *sourcePtr = &source;
    auto stageB = [sourcePtr, &compiled, &columnKeys](
                      RegexByteRange token,
                      internal::WorkerScratch<RegexWorkerState> &worker,
                      KeyIndex &keys,
                      std::span<const internal::TimeColumnSpec> timeColumns,
                      internal::ParsedPipelineBatch &parsed
                  ) {
        DecodeRegexBatch(token, worker, keys, *sourcePtr, timeColumns, parsed, compiled, columnKeys);
    };

    internal::RunStaticParserPipeline<RegexByteRange, RegexWorkerState>(
        source, sink, options, advanced, stageA, stageB, newKeyBaseline
    );
}

std::string RegexParser::ToString(const LogLine &line) const
{
    const auto values = line.IndexedValues();
    if (values.empty())
    {
        return {};
    }
    // Best effort: regex is not invertible. Concatenate values in
    // KeyId order separated by a single space — same convention as
    // `LogfmtParser::ToString` for monostate / unknown fields.
    std::string out;
    bool first = true;
    for (const auto &kv : values)
    {
        if (std::holds_alternative<std::monostate>(kv.second))
        {
            continue;
        }
        if (!first)
        {
            out.push_back(' ');
        }
        first = false;
        std::visit(
            [&out](const auto &v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, std::monostate>)
                {
                    // Skipped above; here for exhaustiveness.
                }
                else if constexpr (std::is_same_v<T, TimeStamp>)
                {
                    out.append(TimeStampToDateTimeString(v));
                }
                else if constexpr (std::is_same_v<T, std::string_view> || std::is_same_v<T, std::string>)
                {
                    out.append(v);
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                    out.append(v ? "true" : "false");
                }
                else
                {
                    out.append(fmt::format("{}", v));
                }
            },
            kv.second
        );
    }
    return out;
}

const RegexTemplate *DetectRegexTemplate(const std::filesystem::path &file)
{
    return ProbeBuiltinTemplates(file);
}

bool ValidateRegexPattern(std::string_view pattern, std::string &errorOut)
{
    if (pattern.empty())
    {
        errorOut = "Regex parser requires a non-empty pattern.";
        return false;
    }
    CompiledPattern compiled;
    if (!compiled.Compile(pattern, errorOut))
    {
        return false;
    }
    if (!compiled.HasNamedGroups())
    {
        errorOut = "Regex pattern has no named capture groups; nothing to put in columns. Use `(?<Name>...)`.";
        return false;
    }
    return true;
}

} // namespace loglib
