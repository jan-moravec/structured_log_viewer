#include "loglib/parse/parsers/regex_parser.hpp"

#include "loglib/parse/internal/regex_compiled_pattern.hpp"

#include "loglib/core/compact_log_value.hpp"
#include "loglib/core/file_line_source.hpp"
#include "loglib/core/internal/pcre2_support.hpp"
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
#include <mutex>
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

/** @brief The bytes that IsValidBytes and DetectRegexTemplateFromBytes look at. */
constexpr size_t IS_VALID_PROBE_BYTES = PROBE_BYTES_BUDGET;

/**
 * @brief How many non-blank probe lines a template must match to claim a file. Two rule out a single line that happens
 * to look like syslog, without turning down short files.
 */
constexpr size_t IS_VALID_MIN_MATCHES = 2;

/** @brief The most non-blank lines collected for detection, so a huge file costs no more than a small one. */
constexpr size_t IS_VALID_PROBE_MAX_LINES = 8;

// The compiled patterns that format detection uses. They cover the built-in templates and the extra templates set with
// SetExtraRegexTemplates.

struct CompiledTemplate
{
    const RegexTemplate *source = nullptr;
    CompiledPattern compiled;
};

/**
 * @brief The compiled patterns of one version of the template catalog.
 *
 * The snapshot holds the template list it was compiled from, so the source pointers of its patterns stay valid while
 * SetExtraRegexTemplates replaces the catalog on another thread. A reader keeps working with its snapshot until it lets
 * go of it.
 */
struct CompiledProbeSnapshot
{
    /**
     * @brief The template list the patterns were compiled from: the built-in templates by priority, then the extras by
     * priority. It keeps the source pointers in compiled valid.
     */
    std::shared_ptr<const std::vector<RegexTemplate>> source;
    /** @brief The patterns in probe order, which is the order of source without the templates that skip detection. */
    std::vector<CompiledTemplate> compiled;
    /** @brief The catalog version the snapshot was built from. A newer version makes the probe build a new snapshot. */
    uint64_t generation = 0;
};

/**
 * @brief The snapshot of the current catalog, built on first use and again after SetExtraRegexTemplates changes it.
 *
 * Compiling every pattern is costly, so all probes share one snapshot until the catalog changes. The probe order is the
 * order of the catalog, which puts the built-in templates before the extras. Sorting by priority alone here would let a
 * user template with a lower priority value take a match from a built-in template.
 */
std::shared_ptr<const CompiledProbeSnapshot> CurrentProbeSnapshot()
{
    // Every probe comes here, and the catalog rarely changes, so the common path only reads the version under a shared
    // lock.
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

    // One thread compiles at a time. Compiling is slow, and the PCRE2 JIT compiler sets up global state on its first
    // call without a lock, so two threads must not compile together.
    static std::mutex buildMutex;
    const std::scoped_lock build(buildMutex);
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

/**
 * @brief True if the pattern matches the whole line.
 *
 * The match is anchored at both ends, so a user template without anchors cannot claim a line where it matches only a
 * part. The built-in templates are anchored already. PatternMatchesLine anchors the same way, and the two must agree,
 * or a template that passes its own check could fail detection. Each call allocates its own match data because the
 * compiled patterns are shared by every thread.
 */
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

/**
 * @brief Returns the first template, in probe order, that matches at least IS_VALID_MIN_MATCHES of the first non-blank
 * lines of sniffBuffer, or nullopt.
 *
 * The match is copied while this function still holds the snapshot. Once it lets go, a SetExtraRegexTemplates call on
 * another thread may free the snapshot and the template in it. RegexParser::IsValidBytes and
 * DetectRegexTemplateFromBytes both use it.
 */
std::optional<RegexTemplate> ProbeAutoDetectTemplates(std::string_view sniffBuffer)
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
        return std::nullopt;
    }

    const auto snapshot = CurrentProbeSnapshot();
    if (!snapshot)
    {
        return std::nullopt;
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
                    return *t.source;
                }
            }
        }
    }
    return std::nullopt;
}

} // namespace

bool RegexParser::IsValidBytes(std::string_view sniffBuffer) const
{
    return ProbeAutoDetectTemplates(sniffBuffer).has_value();
}

std::optional<RegexTemplate> DetectRegexTemplateFromBytes(std::string_view sniffBuffer)
{
    return ProbeAutoDetectTemplates(sniffBuffer);
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
