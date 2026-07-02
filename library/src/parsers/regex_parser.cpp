#include "loglib/parsers/regex_parser.hpp"

#include "loglib/file_line_source.hpp"
#include "loglib/internal/advanced_parser_options.hpp"
#include "loglib/internal/classify_bare_scalar.hpp"
#include "loglib/internal/compact_log_value.hpp"
#include "loglib/internal/line_decoder.hpp"
#include "loglib/internal/regex_template_probe_list.hpp"
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
#include <shared_mutex>
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

/// PCRE2 backtracking limits applied to every match. The defaults
/// (10M / 10M) are too permissive for untrusted patterns —
/// something like `.*.*.*` against a long line could otherwise
/// stall the parse for seconds. These values leave well-behaved
/// patterns (lnav / grok) untouched and surface a `ParsedLineError`
/// on pathological inputs.
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
/// (PCRE2 returns them alphabetically via
/// `pcre2_pattern_info(PCRE2_INFO_NAMETABLE, ...)`). The KeyId at
/// `[i]` is `keys.GetOrInsert(groupNames[i])`; workers index into
/// the ovector at `groupIndices[i] * 2`. Source order is
/// consumer-visible in two places:
///  - column order in `LogTable` follows KeyId allocation, so
///    columns appear left-to-right as the user wrote them (e.g.
///    CLF: `clientip ident auth timestamp verb ...`, not
///    alphabetical `auth bytes clientip ...`).
///  - `RegexParser::ToString` joins values in source order so a
///    regenerated line has its fields in the same slots as the
///    original.
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

        // Re-sort into pattern-source order. PCRE2 gives us the
        // name table sorted alphabetically by name; sorting by
        // group index here is what makes columns and `ToString`
        // follow the order the user wrote (see `PatternSchema`
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

// ---------------------------------------------------------------------
// Auto-detect template compile cache (used by IsValid /
// DetectRegexTemplate). Covers both built-ins and any user
// templates registered via `SetExtraRegexTemplates`.
// ---------------------------------------------------------------------

struct CompiledTemplate
{
    const RegexTemplate *source = nullptr;
    CompiledPattern compiled;
};

/// Compiled snapshot of the merged regex-template registry,
/// pinned to a specific generation. `source` keeps the underlying
/// `RegexTemplate` storage alive so the `CompiledTemplate::source`
/// pointers stay valid even if a concurrent
/// `SetExtraRegexTemplates` call rebuilds the registry — readers
/// using this snapshot keep working until they drop their reference.
struct CompiledProbeSnapshot
{
    /// The merged template list this snapshot was compiled
    /// against: built-ins first (sorted by `priority`) then extras
    /// (sorted by `priority`), matching `regex_templates.cpp`.
    /// Retained to keep `compiled[i].source` valid.
    std::shared_ptr<const std::vector<RegexTemplate>> source;
    /// Compiled patterns in probe order. `autoDetect=false`
    /// templates are excluded entirely; the rest preserve source
    /// ordering (see the two-tier invariant on
    /// `SetExtraRegexTemplates`).
    std::vector<CompiledTemplate> compiled;
    /// `internal::TemplatesGeneration()` value this snapshot was
    /// built against. The probe re-acquires when the counter
    /// advances.
    uint64_t generation = 0;
};

/// Lazy, thread-safe singleton that mirrors the merged template
/// registry. Built on first `IsValid` / `DetectRegexTemplate` and
/// rebuilt whenever `SetExtraRegexTemplates` bumps the generation
/// counter. The compile cost (one `pcre2_compile` +
/// `pcre2_jit_compile` per entry) is amortised across every probe
/// between rebuilds.
///
/// Probe order matches the source list exactly (excluding
/// `autoDetect=false` entries). `regex_templates.cpp` already
/// delivers the list as (built-ins by priority, then extras by
/// priority); re-sorting here by `priority` alone would break the
/// two-tier invariant on `SetExtraRegexTemplates` — a user
/// template with a smaller priority would probe first and
/// silently steal matches from a shipped template.
std::shared_ptr<const CompiledProbeSnapshot> CurrentProbeSnapshot()
{
    // Read-mostly in steady state: the parser hot path re-enters
    // on every probe, so the fast path is a single shared_ptr load
    // + atomic counter read.
    static std::shared_mutex mutex;
    static std::shared_ptr<const CompiledProbeSnapshot> cached;

    const uint64_t generation = internal::TemplatesGeneration();
    {
        std::shared_lock<std::shared_mutex> read(mutex);
        if (cached && cached->generation == generation)
        {
            return cached;
        }
    }

    auto source = internal::MergedRegexTemplates();
    auto fresh = std::make_shared<CompiledProbeSnapshot>();
    fresh->source = source;
    fresh->generation = generation;
    if (source)
    {
        fresh->compiled.reserve(source->size());
        for (const RegexTemplate &tmpl : *source)
        {
            if (!tmpl.autoDetect)
            {
                continue;
            }
            CompiledTemplate c;
            c.source = &tmpl;
            std::string err;
            (void)c.compiled.Compile(tmpl.pattern, err);
            // A failed compile here is a programmer error in the
            // registry. Drop the entry rather than crash so
            // auto-detect keeps working for the rest.
            if (c.compiled.IsReady())
            {
                fresh->compiled.push_back(std::move(c));
            }
        }
    }

    std::unique_lock<std::shared_mutex> write(mutex);
    // Re-check: another thread may have rebuilt against the same
    // generation between the read lock and the write lock. Prefer
    // their snapshot so two probes don't see different compiled
    // instances of the same template (harmless either way,
    // cheaper to skip our copy).
    if (cached && cached->generation == generation)
    {
        return cached;
    }
    cached = fresh;
    return cached;
}

/// True iff @p code matches @p line in full. `PCRE2_ANCHORED |
/// PCRE2_ENDANCHORED` is passed unconditionally so an unanchored
/// user template (no explicit `^...$`) cannot claim a probe match
/// on a substring — auto-detect must mean "the whole line is a
/// record for this template", never "the pattern appears somewhere
/// in the line". The shipped built-ins are all `^...$`-anchored,
/// so this flag pair is a no-op for them; it exists to keep user
/// templates honest. `PatternMatchesLine` (Validate button in the
/// editor) uses the same flags — the two callers **must** stay
/// aligned, otherwise a template that self-tests green could fail
/// the probe (or vice-versa). A per-call `pcre2_match_data` is
/// used because the cache is process-wide; allocating one tiny
/// block is cheap next to the match itself.
bool MatchesFullyForProbe(const CompiledPattern &cp, std::string_view line)
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
        PCRE2_ANCHORED | PCRE2_ENDANCHORED,
        md.get(),
        // `Context()` is logically const here (we only read limits);
        // PCRE2 requires the non-const overload.
        const_cast<pcre2_match_context *>(cp.Context())
    );
    return rc > 0;
}

/// UTF-8 BOM. Some editors (Notepad, older PowerShell) prepend it
/// to text files; with the BOM intact the `^date` / `^IP` / `^[`
/// anchors in the built-in templates can't bind to position 0 and
/// auto-detect silently refuses every file. Stripped from the first
/// probe line only, leaving the rest of the byte stream untouched.
constexpr std::string_view UTF8_BOM = "\xEF\xBB\xBF";

/// File-level probe shared by `IsValid` and `DetectRegexTemplate`.
/// Walks the merged auto-detect registry (built-ins + any
/// `autoDetect=true` extras from `SetExtraRegexTemplates`) in
/// probe order and returns the first entry that matches at least
/// `IS_VALID_MIN_MATCHES` of the first ~16 KiB of non-blank lines,
/// or nullptr. Built-ins probe before user templates by
/// construction — see `CompiledProbeSnapshot`.
const RegexTemplate *ProbeAutoDetectTemplates(const std::filesystem::path &file)
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
        // Pattern matching is brittle on a 1-line file; refuse
        // rather than declare a winner from a coin flip.
        return nullptr;
    }

    const auto snapshot = CurrentProbeSnapshot();
    if (!snapshot)
    {
        return nullptr;
    }
    for (const CompiledTemplate &t : snapshot->compiled)
    {
        size_t hits = 0;
        for (const std::string &l : probeLines)
        {
            if (MatchesFullyForProbe(t.compiled, l))
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

/// Run one `pcre2_match` against @p line and emit the captured
/// named groups as compact values. `out` is appended in source
/// order (caller sorts before constructing the `LogLine`). The
/// decoder reuses `ClassifyBareScalar` so numeric captures get
/// typed (status codes, byte counts, ...), matching CSV's
/// bare-cell typing.
///
/// `fileBegin`/`fileSize` enable the zero-copy `MmapSlice` fast
/// path for static parsing; streaming callers pass `nullptr`/0.
/// `errorOut` is populated on no-match / match-limit / other
/// errors and the function returns false; the caller decides
/// whether to surface it as a `ParsedLineError` or a
/// `LineDecodeResult::Error`.
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
    // Self-contained contract: callers previously had to clear
    // `out` and `errorOut` themselves — brittle if any future
    // caller ever forgets on an error iteration. Clearing here
    // is a no-op for the current hot paths (both static and
    // streaming clear locally) but doesn't trust the call site.
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
            // participate are simply absent from the row; lookups
            // return monostate.
            continue;
        }
        const std::string_view captured(line.data() + startOff, endOff - startOff);
        if (captured.empty())
        {
            // Same convention as CSV: an empty capture is
            // "field present but blank" — drop it so it doesn't
            // bloat the per-line array.
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

    /// Lazily attach to @p compiled on first use.
    /// `enumerable_thread_specific` default-constructs us, so we
    /// can't allocate at construction: the worker doesn't yet
    /// know which compiled pattern to size against.
    void Ensure(const CompiledPattern &compiled)
    {
        if (!matchData)
        {
            matchData = compiled.NewMatchData();
        }
    }
};

/// Stage A token: a contiguous mmap range covering complete
/// lines. Identical to `CsvByteRange`; kept separate so a future
/// regex-specific per-token field stays decoupled from CSV.
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
/// helper; only valid on the very first line of a file.
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
        // Strip a leading UTF-8 BOM only when this line starts at
        // file byte 0. Keeps `^...` anchors in user / built-in
        // patterns binding after a BOM-prefixed editor save.
        // Stage A emits batches on line boundaries, so only the
        // first line of batch 0 can ever match here.
        if (lineStart == fileBegin)
        {
            line = StripBom(line);
        }

        // Pad by 1 on an unterminated last line so the final
        // byte isn't lost on `LogFile::GetLine` round-trips
        // (matches the CSV pipeline).
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

        // `LogLine` ctor asserts ascending KeyIds; the name-table
        // ordering here is source order, not KeyId order.
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
        // Strip a leading UTF-8 BOM only on the first line of the
        // stream. The flag latches so a literal `\xEF\xBB\xBF`
        // inside a later line stays intact.
        if (!mSawFirstLine)
        {
            line = StripBom(line);
            mSawFirstLine = true;
        }
        if (line.empty())
        {
            // The streaming loop already filters blank lines;
            // this branch is defensive. `Skip` (no row, no error)
            // is the right result if it's ever reached.
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

/// Resolve the pattern for this parse. `explicitPattern` (from
/// the pinned-pattern ctor or advanced overload) wins; otherwise
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
/// `OnBatch` before `OnFinished`) when we can't start a pipeline
/// at all — empty pattern, bad pattern, missing named groups.
/// @p streaming picks flush thresholds that match the surrounding
/// pipeline. Both presets behave identically for a single-batch
/// error pass, but the symmetry keeps call sites consistent.
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
    return ProbeAutoDetectTemplates(file) != nullptr;
}

void RegexParser::ParseStreaming(StreamLineSource &source, LogParseSink &sink, ParserOptions options) const
{
    // Capture the `KeyIndex` cursor *before* any intern happens.
    // Forwarded to `EmitErrorAndFinish` on error paths so sink
    // new-key bookkeeping starts at the right place even on
    // failure, and to `RunStreamingParseLoop` on success so
    // pre-interned named-group columns still surface as `newKeys`
    // on the first emitted batch. Without this baseline
    // `BatchCoalescer` would start counting from the post-intern
    // size and `LogTable::AppendBatch` would never create the
    // columns — live tail and network streams would ingest rows
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
    // Forward `mExplicitPattern` as `optional<string_view>` (not
    // `string_view`) so the advanced overload can distinguish
    // "no pattern pinned" (fall back to options) from "pinned to
    // empty" (fail closed). Collapsing both to an empty
    // `string_view` would let a parser explicitly built with `""`
    // silently pick up the configuration's `regexPattern`,
    // diverging from the streaming overload's behaviour.
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

    // Compile once on the orchestrator thread; the pipeline
    // shares the compiled code read-only across Stage B workers.
    // `RunStaticParserPipeline` runs synchronously (joins every
    // TBB worker before returning) so a stack value captured by
    // reference outlives all concurrent uses — no shared_ptr or
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
    // Best effort: regex isn't invertible. Concatenate values in
    // KeyId order separated by a single space — same convention
    // as `LogfmtParser::ToString` for monostate / unknown fields.
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

std::optional<RegexTemplate> DetectRegexTemplate(const std::filesystem::path &file)
{
    // Copy the matched template out of the snapshot before
    // returning so the caller can outlive it without worrying
    // about a concurrent `SetExtraRegexTemplates` invalidating a
    // raw pointer. `RegexTemplate` is a handful of short strings
    // — the copy cost is negligible next to the file I/O the
    // probe just paid for.
    if (const RegexTemplate *tmpl = ProbeAutoDetectTemplates(file); tmpl != nullptr)
    {
        return *tmpl;
    }
    return std::nullopt;
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

bool PatternMatchesLine(std::string_view pattern, std::string_view line)
{
    if (pattern.empty())
    {
        return false;
    }
    CompiledPattern compiled;
    std::string ignoredError;
    if (!compiled.Compile(pattern, ignoredError))
    {
        return false;
    }
    Pcre2MatchDataPtr matchData = compiled.NewMatchData();
    if (!matchData)
    {
        return false;
    }
    // `PCRE2_ANCHORED | PCRE2_ENDANCHORED` mirrors the probe loop
    // (see `MatchesFullyForProbe` above): a partial mid-line
    // match isn't what a `RegexParser` would emit for this line,
    // so it should fail the self-test too. The two callers
    // **must** stay in sync — loosen one, loosen the other, or
    // Validate would pass while the probe fails (or vice-versa).
    const int rc = pcre2_match(
        compiled.Code(),
        reinterpret_cast<PCRE2_SPTR>(line.data()),
        line.size(),
        /*startoffset*/ 0,
        PCRE2_ANCHORED | PCRE2_ENDANCHORED,
        matchData.get(),
        const_cast<pcre2_match_context *>(compiled.Context())
    );
    return rc >= 0;
}

} // namespace loglib
