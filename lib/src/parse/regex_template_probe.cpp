#include "loglib/parse/parsers/regex_parser.hpp"

#include "loglib/parse/internal/regex_compiled_pattern.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/file_line_source.hpp"
#include "loglib/core/internal/pcre2_support.hpp"
#include "loglib/core/log_file.hpp"
#include "loglib/core/log_line.hpp"
#include "loglib/core/stream_line_source.hpp"
#include "loglib/parse/internal/classify_bare_scalar.hpp"
#include "loglib/parse/internal/line_decoder.hpp"
#include "loglib/parse/internal/probe_line_view.hpp"
#include "loglib/parse/internal/regex_template_probe_list.hpp"
#include "loglib/parse/internal/static_parser_pipeline.hpp"
#include "loglib/parse/internal/streaming_parse_loop.hpp"
#include "loglib/parse/log_parser.hpp"
#include "loglib/parse/regex_templates.hpp"
#include "loglib/schema/log_configuration.hpp"

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

using internal::CompiledPattern;
using internal::Pcre2CodePtr;
using internal::Pcre2MatchContextPtr;
using internal::Pcre2MatchDataPtr;
using internal::UTF8_BOM;

/// Bytes scanned by `IsValidBytes` and
/// `DetectRegexTemplateFromBytes`.
constexpr size_t IS_VALID_PROBE_BYTES = PROBE_BYTES_BUDGET;

/// Minimum number of non-blank probe lines a template must match for
/// auto-detection to claim the file. Two is enough to rule out the
/// "one ad-hoc line that happens to look like syslog" case without
/// disqualifying short files.
constexpr size_t IS_VALID_MIN_MATCHES = 2;

/// Maximum number of non-blank probe lines collected before
/// running the auto-detect template loop. Bounded so a huge
/// mmap-friendly file doesn't blow up scratch memory for a
/// yes/no decision.
constexpr size_t IS_VALID_PROBE_MAX_LINES = 8;

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
        const std::shared_lock<std::shared_mutex> read(mutex);
        if (cached && cached->generation == generation)
        {
            return cached;
        }
    }

    const auto source = internal::MergedRegexTemplates();
    const auto fresh = std::make_shared<CompiledProbeSnapshot>();
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

    const std::unique_lock<std::shared_mutex> write(mutex);
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
        cp.Context()
    );
    return rc > 0;
}

/// Shared by `RegexParser::IsValidBytes` and
/// `DetectRegexTemplateFromBytes`.
/// Walks the merged auto-detect registry (built-ins + any
/// `autoDetect=true` extras from `SetExtraRegexTemplates`) in
/// probe order and returns the first entry that matches at least
/// `IS_VALID_MIN_MATCHES` of the first non-blank lines from
/// @p sniffBuffer, or nullptr. Built-ins probe before user
/// templates by construction — see `CompiledProbeSnapshot`.
const RegexTemplate *ProbeAutoDetectTemplates(std::string_view sniffBuffer)
{
    std::vector<std::string> probeLines;
    probeLines.reserve(IS_VALID_PROBE_MAX_LINES);
    size_t cursor = 0;
    size_t bytesScanned = 0;
    bool firstLine = true;
    while (cursor < sniffBuffer.size())
    {
        const internal::ProbeLine probe = internal::NextProbeLine(sniffBuffer, cursor);
        cursor = probe.nextOffset;
        bytesScanned += probe.bytesConsumed;
        std::string_view lineView = probe.line;
        if (firstLine && lineView.starts_with(UTF8_BOM))
        {
            lineView.remove_prefix(UTF8_BOM.size());
        }
        firstLine = false;
        if (!lineView.empty())
        {
            probeLines.emplace_back(lineView);
        }
        if (bytesScanned >= IS_VALID_PROBE_BYTES || probeLines.size() >= IS_VALID_PROBE_MAX_LINES)
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

} // namespace

bool RegexParser::IsValidBytes(std::string_view sniffBuffer) const
{
    return ProbeAutoDetectTemplates(sniffBuffer) != nullptr;
}

std::optional<RegexTemplate> DetectRegexTemplateFromBytes(std::string_view sniffBuffer)
{
    if (const RegexTemplate *tmpl = ProbeAutoDetectTemplates(sniffBuffer); tmpl != nullptr)
    {
        return *tmpl;
    }
    return std::nullopt;
}

std::optional<RegexTemplate> DetectRegexTemplate(const std::filesystem::path &file)
{
    // Copy the matched template out of the snapshot before
    // returning so the caller can outlive it without worrying
    // about a concurrent `SetExtraRegexTemplates` invalidating a
    // raw pointer. `RegexTemplate` is a handful of short strings
    // — the copy cost is negligible next to the file I/O the
    // probe just paid for.
    const std::string head = ReadProbeHead(file, IS_VALID_PROBE_BYTES);
    if (head.empty())
    {
        return std::nullopt;
    }
    return DetectRegexTemplateFromBytes(head);
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

bool ValidateHeaderAnchor(std::string_view anchor, std::string &errorOut)
{
    // Success is represented by an empty error string.
    errorOut.clear();
    if (anchor.empty())
    {
        return true;
    }
    // A boolean header probe does not define a capture schema.
    CompiledPattern compiled;
    std::string compileErr;
    if (!compiled.Compile(anchor, compileErr))
    {
        errorOut = "Header anchor compile failed: " + compileErr;
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
    const Pcre2MatchDataPtr matchData = compiled.NewMatchData();
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
        compiled.Context()
    );
    return rc >= 0;
}

} // namespace loglib
