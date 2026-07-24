#include "anchor_manager.hpp"

#include <loglib/theme.hpp>

#include <algorithm>
#include <cstdint>
#include <utility>

AnchorManager::AnchorManager(QObject *parent)
    : QObject(parent)
{
    // Required for `Qt::QueuedConnection` on the anchor signals
    // (`anchorChanged`, `anchorNoteChanged`) -- Qt copies the Key
    // argument into the event queue and needs the type registered.
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
        // Preserve any existing note: recolour must not clobber it.
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
            // Preserve existing note; only the colour flips.
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
        // Notes only exist alongside an anchor colour. Callers that
        // want to attach a note to an unanchored row must anchor it
        // first (`SetAnchor` seeds with an empty note).
        return false;
    }
    // Sanitise here so the one-line invariant is enforced at the
    // manager boundary; UI callers still pre-sanitise for live
    // display but a direct `SetAnchorNote(std::string{"a\nb"})`
    // can't smuggle newlines past us.
    std::string sanitised = SanitiseNote(std::move(note));
    if (it->second.note == sanitised)
    {
        return false;
    }
    it->second.note = std::move(sanitised);
    // Distinct signal from `anchorChanged`: colour-only listeners
    // (histogram, overview rail) don't need to rebuild on note
    // keystrokes.
    emit anchorNoteChanged(key);
    return true;
}

std::string AnchorManager::SanitiseNote(std::string note)
{
    // Manual single-pass rewrite:
    //   - Collapse `\r\n` (Windows line endings) to one space so a
    //     pasted CRLF-terminated line doesn't leave a double gap.
    //   - Fold lone `\r`, `\n`, `\t`, `\v`, `\f`, and `\0` into
    //     single spaces so pasted binary garbage or a stray form
    //     feed can't break the one-line invariant / truncate the
    //     downstream C-string conversion at the null.
    //   - Fold UTF-8 encodings of U+2028 (LINE SEPARATOR,
    //     `E2 80 A8`) and U+2029 (PARAGRAPH SEPARATOR, `E2 80 A9`)
    //     into single spaces: `QLabel` with `Qt::PlainText` +
    //     `wordWrap` renders them as line breaks in the record-
    //     detail subline.
    //   - Trim leading/trailing spaces from the result.
    //
    // Interior runs of spaces are intentionally preserved so a user
    // can still write `"foo  bar"` (two spaces) if they mean to.
    //
    // Doing the CRLF collapse plus per-char fold in one pass keeps
    // the helper allocation-free on the shrink path and dodges the
    // QString round-trip so it's safe to call from non-Qt code
    // paths (e.g. `Replace` on config load).
    std::string out;
    out.reserve(note.size());
    for (std::size_t i = 0; i < note.size(); ++i)
    {
        const char ch = note[i];
        if (ch == '\r' && i + 1 < note.size() && note[i + 1] == '\n')
        {
            out.push_back(' ');
            ++i; // consume the paired '\n'.
            continue;
        }
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch == '\v' || ch == '\f' || ch == '\0')
        {
            out.push_back(' ');
            continue;
        }
        // U+2028 / U+2029: 3-byte sequences `E2 80 A8` / `E2 80 A9`.
        // Fold before appending the lead byte so we can consume the
        // whole sequence in one step; a truncated sequence at the
        // tail falls through to the default push_back and is later
        // dropped by the UTF-8 walkback in the length-cap branch.
        constexpr unsigned char UTF8_2028_LEAD = 0xE2;
        constexpr unsigned char UTF8_2028_CONT1 = 0x80;
        constexpr unsigned char UTF8_2028_CONT2 = 0xA8;
        constexpr unsigned char UTF8_2029_CONT2 = 0xA9;
        if (static_cast<unsigned char>(ch) == UTF8_2028_LEAD && i + 2 < note.size() &&
            static_cast<unsigned char>(note[i + 1]) == UTF8_2028_CONT1 &&
            (static_cast<unsigned char>(note[i + 2]) == UTF8_2028_CONT2 ||
             static_cast<unsigned char>(note[i + 2]) == UTF8_2029_CONT2))
        {
            out.push_back(' ');
            i += 2; // consume the two continuation bytes.
            continue;
        }
        out.push_back(ch);
    }
    const auto isSpace = [](char ch) { return ch == ' '; };
    const auto firstNonSpace = std::ranges::find_if_not(out, isSpace);
    out.erase(out.begin(), firstNonSpace);
    const auto lastNonSpace = std::ranges::find_if_not(out.rbegin(), out.rend(), isSpace);
    out.erase(lastNonSpace.base(), out.end());

    // Length cap. Truncation walks back to a UTF-8 sequence boundary
    // so a note ending in a multi-byte code point (`é`, `→`, `🌱`)
    // can't be sliced through the continuation bytes -- and cannot
    // leave a lone lead byte at the tail either.
    //
    // Two-step walk:
    //   1. If the first byte *outside* the cap (`out[MAX_NOTE_BYTES]`)
    //      is a continuation byte, walk `truncateAt` back until it
    //      points at a lead byte -- everything from that lead byte
    //      onwards belongs to a sequence that would be truncated.
    //   2. If the last kept byte (`out[truncateAt - 1]`) is itself
    //      a lead byte (top two bits `11`), the sequence it starts
    //      isn't complete within the cap; drop it too.
    //
    // Continuation-byte mask: `0xC0` == `0x80` (top two bits `10`).
    // Lead-byte mask: `0xC0` == `0xC0` (top two bits `11`); ASCII
    // bytes have top bit `0` and fall outside both patterns.
    // Bounded: at most 3 walkback steps for a maximal 4-byte sequence.
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
        // Step 2: if the last byte we're about to keep is a lead byte,
        // its sequence continues past the cap -- drop the lead too.
        if (truncateAt > 0)
        {
            const auto lastKept = static_cast<unsigned char>(out[truncateAt - 1]);
            if ((lastKept & UTF8_TOP_TWO_MASK) == UTF8_LEAD_MARKER)
            {
                --truncateAt;
            }
        }
        out.resize(truncateAt);
        // A trailing space could be exposed by the truncation cut;
        // trim it so the "no trailing whitespace" invariant survives.
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
    std::unordered_map<Key, Value, KeyHash> previous;
    previous.swap(mAnchors);

    std::size_t clampedCount = 0;
    mAnchors.reserve(entries.size());
    for (const loglib::LogConfiguration::AnchorEntry &entry : entries)
    {
        // Clamp unknown palette slots (newer schema / hand-edited)
        // instead of dropping the row: the bookmark position and
        // the note are the parts the user cares about; the exact
        // colour is trivially reassignable via `Ctrl+1..8`. Count
        // the remap so callers can surface a "downgraded" hint.
        auto colorIndex = entry.colorIndex;
        if (colorIndex >= loglib::ANCHOR_PALETTE_SIZE)
        {
            colorIndex = static_cast<uint8_t>(loglib::ANCHOR_PALETTE_SIZE - 1);
            ++clampedCount;
        }
        mAnchors.insert_or_assign(
            Key{.locator = entry.locator, .lineId = entry.lineId},
            // Sanitise on load: a hand-edited JSON with embedded
            // newlines/tabs would otherwise break the "one line"
            // invariant that the QLabel + tooltip rendering paths
            // rely on.
            Value{.colorIndex = colorIndex, .note = SanitiseNote(entry.note)}
        );
    }

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
