#pragma once

#include <loglib/log_configuration.hpp>
#include <loglib/theme.hpp>

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

/// Owns the user's set of anchored rows: `(file, lineId) -> (colour, note)`.
/// One instance per `MainWindow`; `LogModel`, `LogTableView`, and
/// `AnchorsDock` hold a non-owning pointer and react to
/// `anchorChanged` (add / recolour / remove), `anchorNoteChanged`
/// (note text edits), and `anchorsReset` (bulk mutation).
///
/// Key layout:
/// - `locator`: canonical file path (matches `Source::locatorDedupKeys`);
///   empty for in-memory streams.
/// - `lineId`: monotonic parser id, unique within a `LineSource`.
///
/// `colorIndex` indexes `Theme::anchorPalette` (range
/// `[0, loglib::ANCHOR_PALETTE_SIZE)`); `note` is a free-form
/// one-liner (empty by default). Recolouring preserves the note, so
/// `Ctrl+1..8` is non-destructive.
class AnchorManager : public QObject
{
    Q_OBJECT

public:
    /// Composite key. `locator` is owned by value because the
    /// originating `LineSource` may be torn down first (FIFO eviction).
    struct Key
    {
        std::string locator;
        uint64_t lineId = 0;

        friend bool operator==(const Key &, const Key &) = default;
    };

    /// Value carried alongside a `Key`. A struct (not a
    /// `pair<uint8_t, std::string>`) so future per-anchor fields can
    /// be added without churning every call site.
    struct Value
    {
        uint8_t colorIndex = 0;
        std::string note;

        friend bool operator==(const Value &, const Value &) = default;
    };

    explicit AnchorManager(QObject *parent = nullptr);
    ~AnchorManager() override = default;

    AnchorManager(const AnchorManager &) = delete;
    AnchorManager &operator=(const AnchorManager &) = delete;
    AnchorManager(AnchorManager &&) = delete;
    AnchorManager &operator=(AnchorManager &&) = delete;

    /// Add or update an anchor's colour. Out-of-range @p colorIndex
    /// is clamped. Any existing note on @p key is preserved (so
    /// `Ctrl+1..8` doesn't clobber user text). Emits `anchorChanged`
    /// on any state change (fresh insert or colour flip); a no-op
    /// call (identical colour) returns false and suppresses the emit.
    /// Returns true iff state changed.
    bool SetAnchor(const Key &key, uint8_t colorIndex);

    /// Bulk `SetAnchor`. Emits `anchorChanged(key)` for exactly one
    /// mutation, `anchorsReset()` for two or more. Empty @p keys is
    /// a no-op. Same clamping and note-preservation as `SetAnchor`.
    /// Returns true iff anything changed.
    bool SetAnchors(std::span<const Key> keys, uint8_t colorIndex);

    /// Update the one-line note on @p key. No-op (returns false)
    /// when @p key isn't anchored -- notes only exist alongside a
    /// colour. @p note is passed through `SanitiseNote` before
    /// storage so the one-line invariant holds even for direct
    /// callers. Emits `anchorNoteChanged` (not `anchorChanged`) so
    /// colour-only listeners (histogram tick strip, overview rail)
    /// can skip the notification. Returns true iff state changed.
    bool SetAnchorNote(const Key &key, std::string note);

    /// Byte cap (UTF-8) for a single note. Generous for a one-line
    /// annotation, but bounds the worst-case cost of rendering the
    /// note into every tooltip / clipboard payload / dock label so
    /// a megabyte paste can't trickle through the UI. Truncation in
    /// `SanitiseNote` walks back to a UTF-8 sequence boundary; the
    /// silent trim is fine because notes are decorative, not data.
    static constexpr std::size_t MAX_NOTE_BYTES = 1024;

    /// Collapse control characters (CR/LF/tab/...) to spaces, trim
    /// edges, and truncate to `MAX_NOTE_BYTES` at a UTF-8 boundary
    /// so a pasted multi-line block still lands as one tidy line.
    /// Exposed as a static so UI widgets can mirror the stored form
    /// into their editor; the manager applies it on every write, so
    /// callers that don't care about live display can pass raw text.
    [[nodiscard]] static std::string SanitiseNote(std::string note);

    /// Remove an anchor (colour + note). Emits `anchorChanged` iff
    /// something was removed.
    bool RemoveAnchor(const Key &key);

    /// Bulk `RemoveAnchor`. Signal routing mirrors `SetAnchors`.
    /// Missing keys are skipped. Returns true iff anything was removed.
    bool RemoveAnchors(std::span<const Key> keys);

    /// Drop every anchor. Emits `anchorsReset` iff the map was non-empty.
    bool ClearAll();

    /// Replace the entire state from @p entries.
    ///
    /// Out-of-range `colorIndex` values (newer schema or hand-edited
    /// JSON) are clamped to the highest known palette slot rather
    /// than dropped: the bookmark + note are worth keeping, and the
    /// user can reassign the colour trivially. Duplicate keys:
    /// last wins. Each `note` runs through `SanitiseNote` so a
    /// hand-edited JSON can't smuggle multi-line notes past the
    /// one-line invariant.
    ///
    /// `anchorsReset` is emitted only when the rebuilt map differs
    /// from the previous state (silent no-op on identical reload).
    ///
    /// @returns the number of colour-clamped entries, surfaced to
    /// the user by `MainWindow::TryLoadAsConfiguration` via the
    /// status bar so a downgrade is visible.
    [[nodiscard]] std::size_t Replace(const std::vector<loglib::LogConfiguration::AnchorEntry> &entries);

    /// Anchor colour for @p key, or nullopt if not anchored.
    [[nodiscard]] std::optional<uint8_t> ColorFor(const Key &key) const noexcept;

    /// Anchor note for @p key, or nullopt if not anchored. Returns
    /// an empty string when the anchor exists but has no note.
    [[nodiscard]] std::optional<std::string> NoteFor(const Key &key) const;

    /// Snapshot for `LogConfiguration::anchors`, sorted by
    /// `(locator, lineId)` so saved JSON is byte-stable. Each entry
    /// carries its paired note.
    ///
    /// Runtime-only anchors (empty locator) are dropped: their
    /// `lineId` is not stable across sessions and would collide
    /// with unrelated ids on reload. Use
    /// `EntriesIncludingRuntimeOnly` for diagnostics.
    [[nodiscard]] std::vector<loglib::LogConfiguration::AnchorEntry> Entries() const;

    /// Like `Entries` but keeps runtime-only anchors. Diagnostics
    /// and tests only; save paths must use `Entries`.
    [[nodiscard]] std::vector<loglib::LogConfiguration::AnchorEntry> EntriesIncludingRuntimeOnly() const;

    [[nodiscard]] std::size_t Count() const noexcept;

    /// Cheap check for `LogModel::data()` to skip the anchor branch.
    [[nodiscard]] bool Empty() const noexcept;

signals:
    /// One key added, recoloured, or removed. Listeners scope
    /// their repaint to the matching row(s). Note edits go through
    /// `anchorNoteChanged` instead, so colour-only listeners
    /// (histogram, overview rail) can skip this signal.
    void anchorChanged(const AnchorManager::Key &key);

    /// One key's note text changed. Listeners that surface the
    /// note (row tooltip via `LogModel`, anchors dock, record
    /// detail dock) refresh; colour-only listeners skip it.
    void anchorNoteChanged(const AnchorManager::Key &key);

    /// Bulk mutation (`ClearAll`, `Replace`, or a multi-key bulk op).
    /// Listeners refresh the entire visible table.
    void anchorsReset();

private:
    struct KeyHash
    {
        std::size_t operator()(const Key &key) const noexcept
        {
            const std::size_t locatorHash = std::hash<std::string>{}(key.locator);
            const std::size_t lineIdHash = std::hash<uint64_t>{}(key.lineId);
            // boost::hash_combine mix.
            constexpr std::size_t GOLDEN_RATIO_HASH = 0x9E3779B9U;
            constexpr std::size_t LEFT_SHIFT = 6U;
            constexpr std::size_t RIGHT_SHIFT = 2U;
            return locatorHash ^
                   (lineIdHash + GOLDEN_RATIO_HASH + (locatorHash << LEFT_SHIFT) + (locatorHash >> RIGHT_SHIFT));
        }
    };

    /// Shared body of `Entries()` and `EntriesIncludingRuntimeOnly()`.
    /// When @p dropRuntimeOnly is true, entries with an empty
    /// `locator` are excluded. Sort key is `(locator, lineId)` so
    /// on-disk JSON stays byte-stable regardless of hash-map order.
    [[nodiscard]] std::vector<loglib::LogConfiguration::AnchorEntry> BuildSortedEntries(bool dropRuntimeOnly) const;

    std::unordered_map<Key, Value, KeyHash> mAnchors;
};

// Registered so `Qt::QueuedConnection` on the anchor signals can
// copy the Key argument into the event queue.
Q_DECLARE_METATYPE(AnchorManager::Key)
