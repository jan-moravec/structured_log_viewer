#include "loglib/parse/regex_templates.hpp"

#include "loglib/core/error.hpp"
#include "loglib/parse/format_detection.hpp"
#include "loglib/parse/internal/embedded_regex_templates.hpp"
#include "loglib/parse/internal/probe_line_view.hpp"
#include "loglib/parse/internal/regex_compiled_pattern.hpp"
#include "loglib/parse/internal/regex_probe.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

namespace internal
{

std::vector<std::string> CollectProbeLines(std::string_view sample)
{
    std::vector<std::string> lines;
    lines.reserve(PROBE_MAX_LINES);
    std::size_t cursor = 0;
    std::size_t bytesScanned = 0;
    bool firstLine = true;
    while (cursor < sample.size())
    {
        const ProbeLine probe = NextProbeLine(sample, cursor);
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
            lines.emplace_back(lineView);
        }
        if (bytesScanned >= PROBE_BYTES_BUDGET || lines.size() >= PROBE_MAX_LINES)
        {
            break;
        }
    }
    return lines;
}

namespace
{

// PatternMatchesLine in regex_pattern_checks.cpp anchors the same way, and the two must agree, or a template that
// passes its own check could fail detection.
bool MatchesWholeLine(const CompiledPattern &pattern, std::string_view line)
{
    const Pcre2MatchDataPtr matchData = pattern.NewMatchData();
    if (matchData == nullptr)
    {
        return false;
    }
    const int rc = pcre2_match(
        pattern.Code(),
        reinterpret_cast<PCRE2_SPTR>(line.data()),
        line.size(),
        /*startoffset*/ 0,
        PCRE2_ANCHORED | PCRE2_ENDANCHORED,
        matchData.get(),
        pattern.Context()
    );
    return rc > 0;
}

} // namespace

bool PatternFitsLines(const CompiledPattern &pattern, std::span<const std::string> lines)
{
    if (lines.size() < PROBE_MIN_MATCHES)
    {
        // Pattern matching is brittle on a one-line sample, so no pattern wins from a coin flip.
        return false;
    }
    std::size_t hits = 0;
    for (const std::string &line : lines)
    {
        if (MatchesWholeLine(pattern, line) && ++hits >= PROBE_MIN_MATCHES)
        {
            return true;
        }
    }
    return false;
}

} // namespace internal

namespace
{

/**
 * Parses one embedded JSON blob into a RegexTemplate. Throws FormatError on a parse failure, with the source file name
 * in the message. The caller drops that entry and keeps going, so one malformed shipped file cannot stop detection for
 * the rest of the catalog.
 */
RegexTemplate ParseEmbeddedOrThrow(const internal::EmbeddedRegexTemplate &entry)
{
    try
    {
        return ParseRegexTemplate(entry.json);
    }
    catch (const std::exception &ex)
    {
        throw FormatError(
            std::string("Built-in regex template '") + std::string(entry.source) + "' failed to parse: " + ex.what()
        );
    }
}

/**
 * The PCRE2 JIT compiler sets up global state on its first call without a lock, so two threads must not compile
 * patterns together. This mutex guards that set-up and holds no library state.
 */
std::mutex &CompileMutex()
{
    static std::mutex mutex;
    return mutex;
}

struct CompiledProbe
{
    const RegexTemplate *source = nullptr;
    internal::CompiledPattern compiled;
};

} // namespace

struct RegexTemplateCatalog::Data
{
    /** The built-in templates followed by the extras, each group in probe order. */
    std::vector<RegexTemplate> templates;
    std::size_t builtinCount = 0;
    /** One slug per built-in template. */
    std::vector<std::string> slugs;

    /** The compiled patterns of the templates that take part in detection, in probe order. Built on first use. */
    mutable std::once_flag probesOnce;
    mutable std::vector<CompiledProbe> probes;

    [[nodiscard]] const std::vector<CompiledProbe> &Probes() const
    {
        std::call_once(probesOnce, [this] {
            const std::scoped_lock compileLock(CompileMutex());
            probes.reserve(templates.size());
            for (const RegexTemplate &tmpl : templates)
            {
                if (!tmpl.autoDetect)
                {
                    continue;
                }
                CompiledProbe probe;
                probe.source = &tmpl;
                std::string error;
                (void)probe.compiled.Compile(tmpl.regex.pattern, error);
                // A pattern that does not compile cannot match anything. Dropping it keeps detection working for the
                // rest.
                if (probe.compiled.IsReady())
                {
                    probes.push_back(std::move(probe));
                }
            }
        });
        return probes;
    }
};

RegexTemplateCatalog::RegexTemplateCatalog(std::shared_ptr<const Data> data) noexcept
    : mData(std::move(data))
{
}

const RegexTemplateCatalog &RegexTemplateCatalog::Builtin()
{
    static const RegexTemplateCatalog BUILTIN = [] {
        struct Entry
        {
            std::string slug;
            RegexTemplate tmpl;
        };
        std::vector<Entry> entries;
        for (const internal::EmbeddedRegexTemplate &embedded : internal::EmbeddedBuiltinRegexTemplates())
        {
            try
            {
                constexpr std::string_view JSON_SUFFIX = ".json";
                const std::string_view source = embedded.source;
                std::string slug = source.ends_with(JSON_SUFFIX)
                                       ? std::string(source.substr(0, source.size() - JSON_SUFFIX.size()))
                                       : std::string(source);
                entries.push_back(Entry{.slug = std::move(slug), .tmpl = ParseEmbeddedOrThrow(embedded)});
            }
            catch (const std::exception &) // NOLINT(bugprone-empty-catch)
            {
                // A shipped file that does not parse is a programmer error, which the tests of the embedded data catch
                // in CI. The other templates stay usable.
            }
        }
        std::ranges::stable_sort(entries, [](const Entry &a, const Entry &b) {
            return a.tmpl.priority < b.tmpl.priority;
        });

        auto data = std::make_shared<Data>();
        data->templates.reserve(entries.size());
        data->slugs.reserve(entries.size());
        for (Entry &entry : entries)
        {
            data->slugs.push_back(std::move(entry.slug));
            data->templates.push_back(std::move(entry.tmpl));
        }
        data->builtinCount = data->templates.size();
        return RegexTemplateCatalog(std::move(data));
    }();
    return BUILTIN;
}

RegexTemplateCatalog RegexTemplateCatalog::WithExtras(std::vector<RegexTemplate> extras) const
{
    std::ranges::stable_sort(extras, [](const RegexTemplate &a, const RegexTemplate &b) {
        return a.priority < b.priority;
    });
    auto data = std::make_shared<Data>();
    data->builtinCount = mData->builtinCount;
    data->slugs = mData->slugs;
    data->templates.reserve(mData->builtinCount + extras.size());
    const auto builtinEnd = mData->templates.begin() + static_cast<std::ptrdiff_t>(mData->builtinCount);
    data->templates.assign(mData->templates.begin(), builtinEnd);
    std::ranges::move(extras, std::back_inserter(data->templates));
    return RegexTemplateCatalog(std::move(data));
}

std::span<const RegexTemplate> RegexTemplateCatalog::Templates() const noexcept
{
    return mData->templates;
}

std::span<const RegexTemplate> RegexTemplateCatalog::Builtins() const noexcept
{
    return std::span<const RegexTemplate>(mData->templates).first(mData->builtinCount);
}

std::span<const RegexTemplate> RegexTemplateCatalog::Extras() const noexcept
{
    return std::span<const RegexTemplate>(mData->templates).subspan(mData->builtinCount);
}

std::span<const std::string> RegexTemplateCatalog::BuiltinSlugs() const noexcept
{
    return mData->slugs;
}

const RegexTemplate *RegexTemplateCatalog::FindByPattern(std::string_view pattern) const noexcept
{
    for (const RegexTemplate &tmpl : mData->templates)
    {
        if (tmpl.regex.pattern == pattern)
        {
            return &tmpl;
        }
    }
    return nullptr;
}

const RegexTemplate *RegexTemplateCatalog::FindBuiltinBySlug(std::string_view slug) const noexcept
{
    for (std::size_t i = 0; i < mData->slugs.size(); ++i)
    {
        if (mData->slugs[i] == slug)
        {
            return &mData->templates[i];
        }
    }
    return nullptr;
}

const RegexTemplate *RegexTemplateCatalog::Detect(std::string_view sample) const
{
    const std::vector<std::string> lines = internal::CollectProbeLines(sample);
    if (lines.size() < internal::PROBE_MIN_MATCHES)
    {
        return nullptr;
    }
    for (const CompiledProbe &probe : mData->Probes())
    {
        if (internal::PatternFitsLines(probe.compiled, lines))
        {
            return probe.source;
        }
    }
    return nullptr;
}

} // namespace loglib
