#include "anchor_manager.hpp"

#include <loglib/theme.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

AnchorManager::AnchorManager(QObject *parent)
    : QObject(parent)
{
    // Idempotent; Qt guards the underlying registration with a
    // mutex. Without it, a future `Qt::QueuedConnection` to
    // `anchorChanged` would fail with a runtime warning because Qt
    // can't copy the unregistered user type into the event queue.
    qRegisterMetaType<AnchorManager::Key>("AnchorManager::Key");
}

bool AnchorManager::SetAnchor(const Key &key, uint8_t colorIndex)
{
    const auto clamped = static_cast<uint8_t>(std::min<std::size_t>(colorIndex, loglib::ANCHOR_PALETTE_SIZE - 1));

    const auto [it, inserted] = mAnchors.try_emplace(key, clamped);
    if (!inserted)
    {
        if (it->second == clamped)
        {
            return false;
        }
        it->second = clamped;
    }
    emit anchorChanged(key);
    return true;
}

bool AnchorManager::SetAnchors(std::span<const Key> keys, uint8_t colorIndex)
{
    if (keys.empty())
    {
        return false;
    }
    const auto clamped = static_cast<uint8_t>(std::min<std::size_t>(colorIndex, loglib::ANCHOR_PALETTE_SIZE - 1));
    int changeCount = 0;
    // `lastChangedKey` is a pointer into a node already in the map.
    // Per `[unord.req]` (`std::unordered_map`), rehashing (which
    // subsequent `try_emplace` calls in this loop may trigger)
    // invalidates iterators but does NOT invalidate references or
    // pointers to existing elements -- those only invalidate on
    // erase. We never erase here, so the pointer remains valid
    // through the rest of the loop and through the final emit.
    // Mirrors the by-value capture in `RemoveAnchors`, where erase
    // *does* invalidate.
    const Key *lastChangedKey = nullptr;
    for (const Key &key : keys)
    {
        const auto [it, inserted] = mAnchors.try_emplace(key, clamped);
        if (inserted)
        {
            ++changeCount;
            lastChangedKey = &it->first;
            continue;
        }
        if (it->second != clamped)
        {
            it->second = clamped;
            ++changeCount;
            lastChangedKey = &it->first;
        }
    }
    if (changeCount == 0)
    {
        return false;
    }
    // Pick the right signal based on actual mutation count: a single
    // mutation gets the targeted `anchorChanged(key)` so the model
    // overlay only repaints one row, while two-or-more mutations
    // collapse into one `anchorsReset` (cheaper than fanning out
    // N targeted emits to the model + dock). Callers can therefore
    // unconditionally route every selection-driven anchor change
    // through this bulk API and stop branching on `keys.size()`.
    if (changeCount == 1)
    {
        emit anchorChanged(*lastChangedKey);
    }
    else
    {
        emit anchorsReset();
    }
    return true;
}

bool AnchorManager::RemoveAnchor(const Key &key)
{
    const auto it = mAnchors.find(key);
    if (it == mAnchors.end())
    {
        return false;
    }
    // Capture the key before erase: the iterator invalidates on
    // erase, and `emit anchorChanged(key)` callers (e.g. the table
    // model) rely on the locator being live for the duration of
    // the signal dispatch.
    Key removed = it->first;
    mAnchors.erase(it);
    emit anchorChanged(removed);
    return true;
}

bool AnchorManager::RemoveAnchors(std::span<const Key> keys)
{
    if (keys.empty())
    {
        return false;
    }
    int removedCount = 0;
    // Capture the surviving copy of the removed key so the single-
    // mutation signal carries a stable string view -- iterators are
    // invalidated by erase, and the input span may itself be a view
    // onto storage the caller frees after the call returns.
    std::optional<Key> lastRemovedKey;
    for (const Key &key : keys)
    {
        if (mAnchors.erase(key) > 0)
        {
            ++removedCount;
            lastRemovedKey = key;
        }
    }
    if (removedCount == 0)
    {
        return false;
    }
    // Same rationale as `SetAnchors`: collapse to `anchorChanged`
    // on exactly one mutation so the model can use its cheap per-
    // row repaint path; fan out as `anchorsReset` only when two-
    // or-more rows actually changed.
    if (removedCount == 1)
    {
        emit anchorChanged(*lastRemovedKey);
    }
    else
    {
        emit anchorsReset();
    }
    return true;
}

bool AnchorManager::ClearAll()
{
    if (mAnchors.empty())
    {
        return false;
    }
    mAnchors.clear();
    emit anchorsReset();
    return true;
}

std::size_t AnchorManager::Replace(const std::vector<loglib::LogConfiguration::AnchorEntry> &entries)
{
    // Snapshot the pre-clear map so we can short-circuit the
    // `anchorsReset` emit when the rebuilt state matches what we
    // had. Configuration loads (live + autosave) replay this every
    // time, and the common case is "the config we just saved is the
    // config we just loaded" -- no point asking every listener to
    // refresh a thousand rows for that.
    std::unordered_map<Key, uint8_t, KeyHash> previous;
    previous.swap(mAnchors);

    std::size_t droppedCount = 0;
    mAnchors.reserve(entries.size());
    for (const loglib::LogConfiguration::AnchorEntry &entry : entries)
    {
        if (entry.colorIndex >= loglib::ANCHOR_PALETTE_SIZE)
        {
            // Out-of-range colour indices come from a future
            // schema or a hand-edited session JSON; drop them
            // instead of clamping so the user keeps an obvious
            // "anchor disappeared" signal rather than a silent
            // re-colouring to slot 7. The dropped count is
            // surfaced to the caller so the GUI can show a
            // status-bar note rather than swallowing them silently.
            ++droppedCount;
            continue;
        }
        mAnchors.insert_or_assign(Key{.locator = entry.locator, .lineId = entry.lineId}, entry.colorIndex);
    }

    // Compare by content, not by identity. `==` on unordered_map is
    // O(N) but `N` here is "number of anchors the user has set"
    // (typically a handful, capped at a few dozen); the saved
    // `dataChanged` fan-out we avoid is O(rows * cols) over the
    // entire table.
    if (mAnchors != previous)
    {
        emit anchorsReset();
    }
    return droppedCount;
}

std::optional<uint8_t> AnchorManager::ColorFor(const Key &key) const noexcept
{
    const auto it = mAnchors.find(key);
    if (it == mAnchors.end())
    {
        return std::nullopt;
    }
    return it->second;
}

std::vector<loglib::LogConfiguration::AnchorEntry> AnchorManager::Entries() const
{
    std::vector<loglib::LogConfiguration::AnchorEntry> out;
    out.reserve(mAnchors.size());
    for (const auto &[key, colorIndex] : mAnchors)
    {
        // Drop runtime-only anchors (rows whose `LineSource` had an
        // empty `Path()`, e.g. network streams or test fixtures).
        // The `lineId` alone is not stable across sessions for those
        // rows, so persisting them would later collide with arbitrary
        // unrelated `lineId`s in any other session that also lacks a
        // path. See `EntriesIncludingRuntimeOnly` for the unfiltered
        // snapshot used by diagnostics.
        if (key.locator.empty())
        {
            continue;
        }
        out.push_back(loglib::LogConfiguration::AnchorEntry{
            .locator = key.locator,
            .lineId = key.lineId,
            .colorIndex = colorIndex,
        });
    }
    // Stable on-disk order so two consecutive saves of an unchanged
    // anchor set produce byte-identical JSON.
    std::ranges::sort(
        out,
        [](const loglib::LogConfiguration::AnchorEntry &lhs, const loglib::LogConfiguration::AnchorEntry &rhs) {
            if (lhs.locator != rhs.locator)
            {
                return lhs.locator < rhs.locator;
            }
            return lhs.lineId < rhs.lineId;
        }
    );
    return out;
}

std::vector<loglib::LogConfiguration::AnchorEntry> AnchorManager::EntriesIncludingRuntimeOnly() const
{
    std::vector<loglib::LogConfiguration::AnchorEntry> out;
    out.reserve(mAnchors.size());
    for (const auto &[key, colorIndex] : mAnchors)
    {
        out.push_back(loglib::LogConfiguration::AnchorEntry{
            .locator = key.locator,
            .lineId = key.lineId,
            .colorIndex = colorIndex,
        });
    }
    std::ranges::sort(
        out,
        [](const loglib::LogConfiguration::AnchorEntry &lhs, const loglib::LogConfiguration::AnchorEntry &rhs) {
            if (lhs.locator != rhs.locator)
            {
                return lhs.locator < rhs.locator;
            }
            return lhs.lineId < rhs.lineId;
        }
    );
    return out;
}

std::size_t AnchorManager::Count() const noexcept
{
    return mAnchors.size();
}

bool AnchorManager::Empty() const noexcept
{
    return mAnchors.empty();
}
