#include "loglib/regex_templates.hpp"

#include "loglib/internal/embedded_regex_templates.hpp"
#include "loglib/internal/log_configuration_glaze_opts.hpp"
#include "loglib/internal/regex_template_glaze_meta.hpp"
#include "loglib/internal/regex_template_probe_list.hpp"

#include <glaze/glaze.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace loglib
{

namespace
{

/// Parse one embedded JSON blob into a `RegexTemplate`. Throws
/// `std::runtime_error` on parse failure with the source filename
/// woven into the diagnostic. Caller treats the throw as "drop this
/// entry but keep going" so one malformed shipped file can't wedge
/// auto-detection for the rest of the catalog.
RegexTemplate ParseEmbeddedOrThrow(const internal::EmbeddedRegexTemplate &entry)
{
    try
    {
        return ParseRegexTemplate(entry.json);
    }
    catch (const std::exception &ex)
    {
        throw std::runtime_error(
            std::string("Built-in regex template '") + std::string(entry.source) + "' failed to parse: " + ex.what()
        );
    }
}

/// The merged probe registry: built-ins first (sorted by
/// `priority`), then user-supplied extras (also sorted by
/// `priority`). Two-tier order is deliberate so a careless
/// user-priority can never steal a probe match from a shipped
/// template; the priority sort *within* each tier matches the
/// documented "lower probes first" semantics.
struct MergedRegistry
{
    std::vector<RegexTemplate> builtins;
    std::vector<RegexTemplate> extras;
    /// Stable, owning flattened view: built-ins then extras, in
    /// the order the probe loop should see them. A shared_ptr to
    /// *this* lets readers pin the storage for the duration of an
    /// iteration even while a concurrent writer prepares a fresh
    /// `MergedRegistry`.
    std::vector<RegexTemplate> ordered;
};

/// Monotonic generation counter bumped on every registry rebuild.
/// Readers in `parsers/regex_parser.cpp` use this to invalidate
/// their compiled cache without taking a mutex on the hot path.
std::atomic<uint64_t> &GenerationCounter()
{
    static std::atomic<uint64_t> counter{0};
    return counter;
}

std::shared_mutex &RegistryMutex()
{
    static std::shared_mutex m;
    return m;
}

/// Lazily built once on first read and rebuilt whenever
/// `SetExtraRegexTemplates` swaps in a new extras set. Protected by
/// `RegistryMutex()`. Held via `shared_ptr` so a concurrent reader
/// taking a copy under the read lock is unaffected by a subsequent
/// rebuild.
std::shared_ptr<const MergedRegistry> &SharedRegistrySlot()
{
    static std::shared_ptr<const MergedRegistry> slot;
    return slot;
}

/// Snapshot of extras registered via `SetExtraRegexTemplates`.
/// Protected by `RegistryMutex()`. Copied into the next
/// `MergedRegistry` build.
std::vector<RegexTemplate> &ExtrasSlot()
{
    static std::vector<RegexTemplate> v;
    return v;
}

/// Build (or rebuild) the merged registry from the embedded
/// catalog + the current extras. Caller must hold a unique lock on
/// `RegistryMutex()`.
std::shared_ptr<const MergedRegistry> RebuildLocked()
{
    auto fresh = std::make_shared<MergedRegistry>();

    for (const internal::EmbeddedRegexTemplate &entry : internal::EmbeddedBuiltinRegexTemplates())
    {
        try
        {
            fresh->builtins.push_back(ParseEmbeddedOrThrow(entry));
        }
        catch (const std::exception &)
        {
            // Programmer error in the shipped catalog; skip the
            // entry rather than crashing so auto-detect keeps
            // working for the rest. The CI sweep in
            // `test_regex_templates.cpp` would have caught it
            // before this could ever reach a user.
        }
    }
    std::stable_sort(
        fresh->builtins.begin(),
        fresh->builtins.end(),
        [](const RegexTemplate &a, const RegexTemplate &b) { return a.priority < b.priority; }
    );

    fresh->extras = ExtrasSlot();
    std::stable_sort(
        fresh->extras.begin(),
        fresh->extras.end(),
        [](const RegexTemplate &a, const RegexTemplate &b) { return a.priority < b.priority; }
    );

    fresh->ordered.reserve(fresh->builtins.size() + fresh->extras.size());
    for (const RegexTemplate &t : fresh->builtins)
    {
        fresh->ordered.push_back(t);
    }
    for (const RegexTemplate &t : fresh->extras)
    {
        fresh->ordered.push_back(t);
    }

    SharedRegistrySlot() = fresh;
    // Bump the generation counter *after* the slot is updated so
    // the parser, if it reads the counter and then the snapshot,
    // can never see an old snapshot under a new generation.
    GenerationCounter().fetch_add(1, std::memory_order_release);
    return fresh;
}

std::shared_ptr<const MergedRegistry> CurrentRegistry()
{
    {
        std::shared_lock<std::shared_mutex> read(RegistryMutex());
        if (auto snap = SharedRegistrySlot())
        {
            return snap;
        }
    }
    std::unique_lock<std::shared_mutex> write(RegistryMutex());
    // Re-check under the write lock; another thread may have built
    // the registry between dropping the read lock and acquiring the
    // write lock.
    if (auto snap = SharedRegistrySlot())
    {
        return snap;
    }
    return RebuildLocked();
}

} // namespace

RegexTemplate ParseRegexTemplate(std::string_view content)
{
    constexpr auto OPTS = loglib::internal::LOG_CONFIG_OPTS;

    RegexTemplate parsed;
    const auto error = glz::read<OPTS>(parsed, content);
    if (error)
    {
        throw std::runtime_error(
            "Failed to parse regex template JSON: " + glz::format_error(error, std::string(content))
        );
    }
    return parsed;
}

std::string SerializeRegexTemplate(const RegexTemplate &tmpl)
{
    constexpr auto OPTS = loglib::internal::LOG_CONFIG_OPTS;

    std::string json;
    const auto error = glz::write<OPTS>(tmpl, json);
    if (error)
    {
        throw std::runtime_error("Failed to serialise regex template JSON: " + glz::format_error(error));
    }
    return json;
}

std::span<const RegexTemplate> BuiltinRegexTemplates() noexcept
{
    // Pin the registry snapshot for the lifetime of the process so
    // the returned span stays valid regardless of subsequent
    // `SetExtraRegexTemplates` calls (which rebuild the merged
    // registry but never touch the built-in slice the snapshot
    // captures).
    static const std::vector<RegexTemplate> *cached = nullptr;
    static std::once_flag cacheOnce;
    std::call_once(cacheOnce, []() {
        auto snap = CurrentRegistry();
        if (!snap)
        {
            return;
        }
        // The built-in slice is immutable post-construction; cache
        // a raw pointer into its storage so subsequent calls don't
        // re-acquire the mutex. Bumping the snapshot into a
        // never-destroyed static keeps the underlying vector alive.
        static std::shared_ptr<const MergedRegistry> pinned = std::move(snap);
        cached = &pinned->builtins;
    });
    return cached != nullptr ? std::span<const RegexTemplate>(*cached) : std::span<const RegexTemplate>{};
}

void SetExtraRegexTemplates(std::span<const RegexTemplate> extras)
{
    std::unique_lock<std::shared_mutex> write(RegistryMutex());
    ExtrasSlot().assign(extras.begin(), extras.end());
    // Eagerly rebuild so the next probe sees the fresh extras
    // without taking the write lock.
    (void)RebuildLocked();
}

const RegexTemplate *FindTemplateByPattern(std::string_view pattern) noexcept
{
    const auto snap = CurrentRegistry();
    if (!snap)
    {
        return nullptr;
    }
    for (const RegexTemplate &t : snap->ordered)
    {
        if (t.pattern == pattern)
        {
            return &t;
        }
    }
    return nullptr;
}

const RegexTemplate *FindBuiltinByPattern(std::string_view pattern) noexcept
{
    return FindTemplateByPattern(pattern);
}

namespace internal
{

std::shared_ptr<const std::vector<RegexTemplate>> MergedRegexTemplates()
{
    auto snap = CurrentRegistry();
    if (!snap)
    {
        return nullptr;
    }
    // Hand out a `shared_ptr` aliased to the `ordered` vector so
    // the snapshot's reference count keeps the storage alive for
    // the caller without exposing the `MergedRegistry` type.
    return std::shared_ptr<const std::vector<RegexTemplate>>(snap, &snap->ordered);
}

uint64_t TemplatesGeneration() noexcept
{
    return GenerationCounter().load(std::memory_order_acquire);
}

} // namespace internal

} // namespace loglib
