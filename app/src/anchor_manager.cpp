#include "anchor_manager.hpp"

#include <loglib/theme.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

AnchorManager::AnchorManager(QObject *parent)
    : QObject(parent)
{
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

void AnchorManager::Replace(const std::vector<loglib::LogConfiguration::AnchorEntry> &entries)
{
    mAnchors.clear();
    mAnchors.reserve(entries.size());
    for (const loglib::LogConfiguration::AnchorEntry &entry : entries)
    {
        if (entry.colorIndex >= loglib::ANCHOR_PALETTE_SIZE)
        {
            // Out-of-range colour indices come from a future
            // schema or a hand-edited session JSON; drop them
            // instead of clamping so the user keeps an obvious
            // "anchor disappeared" signal rather than a silent
            // re-colouring to slot 7.
            continue;
        }
        mAnchors[Key{.locator = entry.locator, .lineId = entry.lineId}] = entry.colorIndex;
    }
    emit anchorsReset();
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
        out.push_back(loglib::LogConfiguration::AnchorEntry{
            .locator = key.locator,
            .lineId = key.lineId,
            .colorIndex = colorIndex,
        });
    }
    // Stable on-disk order so two consecutive saves of an unchanged
    // anchor set produce byte-identical JSON.
    std::ranges::sort(out, [](const loglib::LogConfiguration::AnchorEntry &lhs, const loglib::LogConfiguration::AnchorEntry &rhs) {
        if (lhs.locator != rhs.locator)
        {
            return lhs.locator < rhs.locator;
        }
        return lhs.lineId < rhs.lineId;
    });
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
