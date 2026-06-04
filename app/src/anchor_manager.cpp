#include "anchor_manager.hpp"

#include <loglib/theme.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

AnchorManager::AnchorManager(QObject *parent)
    : QObject(parent)
{
    // Required for `Qt::QueuedConnection` on the `anchorChanged` signal.
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
    // Pointer-to-element stays valid: rehash doesn't invalidate
    // element addresses and we don't erase here.
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
    // One change -> scoped repaint; many -> single full refresh.
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
    // Copy before erase: listeners need a live string after `it`
    // is invalidated.
    const Key removed = it->first;
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
    // Own a copy so the single-change signal survives the caller's
    // span going out of scope. Default-constructed; only read when
    // `removedCount == 1`, which only the same `if` that updates it
    // makes possible -- bypassing `std::optional` keeps clang-tidy's
    // `bugprone-unchecked-optional-access` quiet without an assertion.
    Key lastRemovedKey;
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
    // Same routing as `SetAnchors`.
    if (removedCount == 1)
    {
        emit anchorChanged(lastRemovedKey);
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
    // Snapshot the previous state so an identical reload stays silent.
    std::unordered_map<Key, uint8_t, KeyHash> previous;
    previous.swap(mAnchors);

    std::size_t droppedCount = 0;
    mAnchors.reserve(entries.size());
    for (const loglib::LogConfiguration::AnchorEntry &entry : entries)
    {
        if (entry.colorIndex >= loglib::ANCHOR_PALETTE_SIZE)
        {
            // Drop unknown-slot entries (newer schema / hand-edited).
            ++droppedCount;
            continue;
        }
        mAnchors.insert_or_assign(Key{.locator = entry.locator, .lineId = entry.lineId}, entry.colorIndex);
    }

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
        // Drop runtime-only anchors (empty locator); their lineId
        // isn't stable across sessions.
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
    // Byte-stable on-disk order.
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
