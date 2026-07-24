#include "anchor_manager.hpp"

#include <loglib/theme.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

AnchorManager::AnchorManager(QObject *parent)
    : QObject(parent)
{
    // Required so `Qt::QueuedConnection` can copy Key across threads.
    qRegisterMetaType<AnchorManager::Key>("AnchorManager::Key");
}

bool AnchorManager::SetAnchor(const Key &key, uint8_t colorIndex)
{
    const auto clamped = static_cast<uint8_t>(std::min<std::size_t>(colorIndex, loglib::ANCHOR_PALETTE_SIZE - 1));

    const auto [it, inserted] = mAnchors.try_emplace(key, Value{.colorIndex = clamped, .note = {}});
    if (!inserted)
    {
        if (it->second.colorIndex == clamped)
        {
            return false;
        }
        // Preserve any existing note; only the colour flips.
        it->second.colorIndex = clamped;
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
        const auto [it, inserted] = mAnchors.try_emplace(key, Value{.colorIndex = clamped, .note = {}});
        if (inserted)
        {
            ++changeCount;
            lastChangedKey = &it->first;
            continue;
        }
        if (it->second.colorIndex != clamped)
        {
            // Preserve the note; only the colour flips.
            it->second.colorIndex = clamped;
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

bool AnchorManager::SetAnchorNote(const Key &key, std::string note)
{
    const auto it = mAnchors.find(key);
    if (it == mAnchors.end())
    {
        // Notes only exist alongside an anchor colour; anchor the
        // row first if you want to attach a note.
        return false;
    }
    // Sanitise at the manager boundary so a direct
    // `SetAnchorNote("a\nb")` can't smuggle newlines past us.
    std::string sanitised = SanitiseNote(std::move(note));
    if (it->second.note == sanitised)
    {
        return false;
    }
    it->second.note = std::move(sanitised);
    // Distinct from `anchorChanged` so colour-only listeners
    // (histogram, overview rail) skip note keystrokes.
    emit anchorNoteChanged(key);
    return true;
}

std::string AnchorManager::SanitiseNote(std::string note)
{
    // Single-pass rewrite that:
    //   - collapses `\r\n` and every C0 control byte (0x00..0x1F,
    //     plus DEL 0x7F) to one space -- covers `\n`, `\t`, `\0`,
    //     BEL, ESC, ... none of which belong in a one-line note;
    //   - folds U+2028 (LINE SEPARATOR) and U+2029 (PARAGRAPH
    //     SEPARATOR) UTF-8 sequences to a space so `QLabel` with
    //     `Qt::PlainText` + `wordWrap` doesn't break on them;
    //   - trims leading and trailing spaces.
    //
    // Interior runs of spaces are preserved so `"foo  bar"` still
    // works. Purely `std::string` so `Replace` can call this on
    // config load without a Qt round-trip.
    constexpr unsigned char C0_UPPER_BOUND = 0x1F;
    constexpr unsigned char ASCII_DEL = 0x7F;
    std::string out;
    out.reserve(note.size());
    for (std::size_t i = 0; i < note.size(); ++i)
    {
        const auto ch = static_cast<unsigned char>(note[i]);
        if (ch == '\r' && i + 1 < note.size() && note[i + 1] == '\n')
        {
            out.push_back(' ');
            ++i;
            continue;
        }
        if (ch <= C0_UPPER_BOUND || ch == ASCII_DEL)
        {
            out.push_back(' ');
            continue;
        }
        // U+2028 / U+2029: 3-byte sequences `E2 80 A8` / `E2 80 A9`.
        // A truncated tail sequence falls through to `push_back` and
        // is cleaned up by the UTF-8 walkback in the length cap.
        constexpr unsigned char UTF8_2028_LEAD = 0xE2;
        constexpr unsigned char UTF8_2028_CONT1 = 0x80;
        constexpr unsigned char UTF8_2028_CONT2 = 0xA8;
        constexpr unsigned char UTF8_2029_CONT2 = 0xA9;
        if (ch == UTF8_2028_LEAD && i + 2 < note.size() &&
            static_cast<unsigned char>(note[i + 1]) == UTF8_2028_CONT1 &&
            (static_cast<unsigned char>(note[i + 2]) == UTF8_2028_CONT2 ||
             static_cast<unsigned char>(note[i + 2]) == UTF8_2029_CONT2))
        {
            out.push_back(' ');
            i += 2;
            continue;
        }
        out.push_back(static_cast<char>(ch));
    }
    const auto isSpace = [](char ch) { return ch == ' '; };
    const auto firstNonSpace = std::ranges::find_if_not(out, isSpace);
    out.erase(out.begin(), firstNonSpace);
    const auto lastNonSpace = std::ranges::find_if_not(out.rbegin(), out.rend(), isSpace);
    out.erase(lastNonSpace.base(), out.end());

    // Length cap. Truncation walks back to a UTF-8 boundary so a
    // multi-byte code point (`é`, `→`, `🌱`) can't be sliced and no
    // lone lead byte is left at the tail:
    //   1. if the byte at `out[MAX_NOTE_BYTES]` is a continuation
    //      byte (top bits `10`), walk `truncateAt` back to the lead
    //      of that sequence;
    //   2. if the last kept byte is itself a lead (top bits `11`),
    //      its sequence extends past the cap -- drop the lead too.
    // Bounded to at most 3 walkback steps (max 4-byte sequence).
    if (out.size() > MAX_NOTE_BYTES)
    {
        constexpr unsigned char UTF8_TOP_TWO_MASK = 0xC0;
        constexpr unsigned char UTF8_CONT_MARKER = 0x80;
        constexpr unsigned char UTF8_LEAD_MARKER = 0xC0;
        std::size_t truncateAt = MAX_NOTE_BYTES;
        while (truncateAt > 0 &&
               (static_cast<unsigned char>(out[truncateAt]) & UTF8_TOP_TWO_MASK) == UTF8_CONT_MARKER)
        {
            --truncateAt;
        }
        if (truncateAt > 0)
        {
            const auto lastKept = static_cast<unsigned char>(out[truncateAt - 1]);
            if ((lastKept & UTF8_TOP_TWO_MASK) == UTF8_LEAD_MARKER)
            {
                --truncateAt;
            }
        }
        out.resize(truncateAt);
        // Truncation can expose a trailing space; re-trim so the
        // "no trailing whitespace" invariant survives.
        const auto lastNonSpaceAfterCut = std::ranges::find_if_not(out.rbegin(), out.rend(), isSpace);
        out.erase(lastNonSpaceAfterCut.base(), out.end());
    }
    return out;
}

bool AnchorManager::RemoveAnchor(const Key &key)
{
    const auto it = mAnchors.find(key);
    if (it == mAnchors.end())
    {
        return false;
    }
    // Copy before erase so the signal argument survives `it`
    // being invalidated.
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
    // Own a copy so the single-change signal outlives the caller's
    // span. Only read when `removedCount == 1`, so
    // default-constructed is fine (and avoids clang-tidy's
    // unchecked-optional-access lint on a `std::optional` variant).
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
    // Snapshot previous state so an identical reload stays silent.
    std::unordered_map<Key, Value, KeyHash> previous;
    previous.swap(mAnchors);

    // Track clamp status per key so the returned count matches the
    // final (last-wins) state on duplicates: a valid entry followed
    // by an out-of-range one counts as clamped, and vice versa.
    std::unordered_map<Key, bool, KeyHash> clampedByKey;
    mAnchors.reserve(entries.size());
    clampedByKey.reserve(entries.size());
    for (const loglib::LogConfiguration::AnchorEntry &entry : entries)
    {
        // Clamp unknown palette slots instead of dropping the
        // anchor: the bookmark + note are what the user cares
        // about; colour is trivially reassignable via `Ctrl+1..8`.
        auto colorIndex = entry.colorIndex;
        const bool clamped = colorIndex >= loglib::ANCHOR_PALETTE_SIZE;
        if (clamped)
        {
            colorIndex = static_cast<uint8_t>(loglib::ANCHOR_PALETTE_SIZE - 1);
        }
        Key key{.locator = entry.locator, .lineId = entry.lineId};
        clampedByKey.insert_or_assign(key, clamped);
        // Sanitise on load so hand-edited JSON can't smuggle
        // multi-line notes past the one-line invariant.
        mAnchors.insert_or_assign(
            std::move(key), Value{.colorIndex = colorIndex, .note = SanitiseNote(entry.note)}
        );
    }
    const auto clampedCount =
        static_cast<std::size_t>(std::ranges::count_if(clampedByKey, [](const auto &pair) { return pair.second; }));

    if (mAnchors != previous)
    {
        emit anchorsReset();
    }
    return clampedCount;
}

std::optional<uint8_t> AnchorManager::ColorFor(const Key &key) const noexcept
{
    const auto it = mAnchors.find(key);
    if (it == mAnchors.end())
    {
        return std::nullopt;
    }
    return it->second.colorIndex;
}

std::optional<std::string> AnchorManager::NoteFor(const Key &key) const
{
    const auto it = mAnchors.find(key);
    if (it == mAnchors.end())
    {
        return std::nullopt;
    }
    return it->second.note;
}

std::vector<loglib::LogConfiguration::AnchorEntry> AnchorManager::BuildSortedEntries(bool dropRuntimeOnly) const
{
    std::vector<loglib::LogConfiguration::AnchorEntry> out;
    out.reserve(mAnchors.size());
    for (const auto &[key, value] : mAnchors)
    {
        if (dropRuntimeOnly && key.locator.empty())
        {
            continue;
        }
        out.push_back(
            loglib::LogConfiguration::AnchorEntry{
                .locator = key.locator,
                .lineId = key.lineId,
                .colorIndex = value.colorIndex,
                .note = value.note,
            }
        );
    }
    std::ranges::sort(
        out, [](const loglib::LogConfiguration::AnchorEntry &lhs, const loglib::LogConfiguration::AnchorEntry &rhs) {
            if (lhs.locator != rhs.locator)
            {
                return lhs.locator < rhs.locator;
            }
            return lhs.lineId < rhs.lineId;
        }
    );
    return out;
}

std::vector<loglib::LogConfiguration::AnchorEntry> AnchorManager::Entries() const
{
    return BuildSortedEntries(/*dropRuntimeOnly=*/true);
}

std::vector<loglib::LogConfiguration::AnchorEntry> AnchorManager::EntriesIncludingRuntimeOnly() const
{
    return BuildSortedEntries(/*dropRuntimeOnly=*/false);
}

std::size_t AnchorManager::Count() const noexcept
{
    return mAnchors.size();
}

bool AnchorManager::Empty() const noexcept
{
    return mAnchors.empty();
}
