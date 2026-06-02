#pragma once

#include <loglib/log_configuration.hpp>
#include <loglib/theme.hpp>

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

/// Persistent owner of the user-marked "anchor" set: the per-row
/// (file, lineId) -> colour-index map driven by the right-click
/// menu, the `Ctrl+1..8` / `Ctrl+0` hotkeys, and the Anchors dock.
///
/// One instance per `MainWindow`. Held by non-owning pointer in
/// `LogModel`, `LogTableView`, and `AnchorsDock`; each connects to
/// `anchorChanged` / `anchorsReset` and scopes its repaint /
/// refresh accordingly.
///
/// The map's key is `(locator, lineId)`:
/// - `locator` is the canonicalised file path (the same form used
///   for `LogConfiguration::Source::locatorDedupKeys`). Single-file
///   sessions and in-memory streams pass an empty locator; the app
///   layer is responsible for canonicalising consistently across
///   call sites.
/// - `lineId` is the monotonic id assigned by the parser; survives
///   FIFO eviction within a session and persists across re-opens
///   of the same recent entry.
///
/// `colorIndex` indexes into the active theme's `anchorPalette`
/// (or the app's built-in fallback). Valid range is
/// `[0, loglib::ANCHOR_PALETTE_SIZE)`.
class AnchorManager : public QObject
{
    Q_OBJECT

public:
    /// Composite key. `locator` lives by value so the manager owns
    /// the string; the `LineSource` it came from may be torn down
    /// before the anchor is dropped (FIFO eviction, end of stream).
    struct Key
    {
        std::string locator;
        uint64_t lineId = 0;

        friend bool operator==(const Key &, const Key &) = default;
    };

    explicit AnchorManager(QObject *parent = nullptr);
    ~AnchorManager() override = default;

    AnchorManager(const AnchorManager &) = delete;
    AnchorManager &operator=(const AnchorManager &) = delete;
    AnchorManager(AnchorManager &&) = delete;
    AnchorManager &operator=(AnchorManager &&) = delete;

    /// Add or update an anchor. Out-of-range @p colorIndex is
    /// clamped into `[0, ANCHOR_PALETTE_SIZE)` so the GUI cannot
    /// accidentally produce a slot the resolver would treat as
    /// "use fallback". Emits `anchorChanged` exactly when the
    /// stored state changes (no-op when @p key already maps to
    /// @p colorIndex).
    bool SetAnchor(const Key &key, uint8_t colorIndex);

    /// Remove an anchor by key. Emits `anchorChanged` when an
    /// entry was actually removed. Returns true iff something
    /// changed.
    bool RemoveAnchor(const Key &key);

    /// Drop every anchor in one shot. Emits `anchorsReset` exactly
    /// when the map was non-empty.
    bool ClearAll();

    /// Bulk replace -- used by `ApplyLoadedConfiguration`. Drops
    /// the current state and rebuilds from @p entries; out-of-range
    /// `colorIndex` entries are silently skipped. Emits
    /// `anchorsReset` unconditionally so dependent views refresh
    /// regardless of whether anything changed.
    void Replace(const std::vector<loglib::LogConfiguration::AnchorEntry> &entries);

    /// @returns the anchor colour for @p key, or nullopt if @p key
    /// is not anchored.
    [[nodiscard]] std::optional<uint8_t> ColorFor(const Key &key) const noexcept;

    /// Snapshot suitable for `LogConfiguration::anchors`. Sorted
    /// by `(locator, lineId)` so on-disk JSON diffs are stable
    /// from one save to the next; in-memory order has no meaning
    /// otherwise.
    [[nodiscard]] std::vector<loglib::LogConfiguration::AnchorEntry> Entries() const;

    /// Live count.
    [[nodiscard]] std::size_t Count() const noexcept;

    /// True iff there are no anchors at all. Cheap check for the
    /// model's `data()` hot path.
    [[nodiscard]] bool Empty() const noexcept;

signals:
    /// Single-key mutation (add / colour-change / remove). Listeners
    /// scope their `dataChanged` emit to the matching row(s).
    void anchorChanged(const AnchorManager::Key &key);

    /// Bulk mutation (`ClearAll`, `Replace`). Listeners refresh
    /// the entire visible table; cheaper than emitting one signal
    /// per key for the load-time fan-out.
    void anchorsReset();

private:
    struct KeyHash
    {
        std::size_t operator()(const Key &key) const noexcept
        {
            const std::size_t locatorHash = std::hash<std::string>{}(key.locator);
            const std::size_t lineIdHash = std::hash<uint64_t>{}(key.lineId);
            // Mix via xor + golden-ratio shift; matches the
            // boost::hash_combine idiom used elsewhere in the
            // codebase. The shift amounts are the published values
            // from Boost's reference implementation; renaming them
            // wouldn't add meaning.
            constexpr std::size_t GOLDEN_RATIO_HASH = 0x9E3779B9U;
            constexpr std::size_t LEFT_SHIFT = 6U;
            constexpr std::size_t RIGHT_SHIFT = 2U;
            return locatorHash ^
                   (lineIdHash + GOLDEN_RATIO_HASH + (locatorHash << LEFT_SHIFT) + (locatorHash >> RIGHT_SHIFT));
        }
    };

    std::unordered_map<Key, uint8_t, KeyHash> mAnchors;
};
