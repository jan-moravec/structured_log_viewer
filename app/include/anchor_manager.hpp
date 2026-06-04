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

/// Owns the user's set of anchored rows: `(file, lineId) -> colour-index`.
/// One instance per `MainWindow`; `LogModel`, `LogTableView`, and
/// `AnchorsDock` hold a non-owning pointer and react to
/// `anchorChanged` / `anchorsReset`.
///
/// Key layout:
/// - `locator`: canonical file path (matches `Source::locatorDedupKeys`).
///   Empty for in-memory streams.
/// - `lineId`: monotonic id from the parser, unique within a `LineSource`.
///
/// `colorIndex` indexes `Theme::anchorPalette` (range
/// `[0, loglib::ANCHOR_PALETTE_SIZE)`).
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

    explicit AnchorManager(QObject *parent = nullptr);
    ~AnchorManager() override = default;

    AnchorManager(const AnchorManager &) = delete;
    AnchorManager &operator=(const AnchorManager &) = delete;
    AnchorManager(AnchorManager &&) = delete;
    AnchorManager &operator=(AnchorManager &&) = delete;

    /// Add or update an anchor. Out-of-range @p colorIndex is clamped.
    /// Emits `anchorChanged` only when the stored colour actually changes.
    /// Returns true iff state changed.
    bool SetAnchor(const Key &key, uint8_t colorIndex);

    /// Bulk `SetAnchor`. Emits `anchorChanged(key)` for exactly one
    /// mutation; `anchorsReset()` for two or more. Empty @p keys is a
    /// no-op. Same clamping as `SetAnchor`. Returns true iff anything
    /// changed.
    bool SetAnchors(std::span<const Key> keys, uint8_t colorIndex);

    /// Remove an anchor. Emits `anchorChanged` iff something was removed.
    bool RemoveAnchor(const Key &key);

    /// Bulk `RemoveAnchor`. Signal routing mirrors `SetAnchors`.
    /// Missing keys are skipped. Returns true iff anything was removed.
    bool RemoveAnchors(std::span<const Key> keys);

    /// Drop every anchor. Emits `anchorsReset` iff the map was non-empty.
    bool ClearAll();

    /// Replace the entire state from @p entries.
    ///
    /// Entries with an out-of-range `colorIndex` (newer schema or
    /// hand-edited JSON) are dropped rather than clamped, so missing
    /// anchors stay visible to the user. Duplicate keys: last wins.
    ///
    /// `anchorsReset` is emitted only when the rebuilt map differs
    /// from the previous content (silent no-op on identical reload).
    ///
    /// @returns the number of dropped entries; surfaced to the user
    /// by `MainWindow::TryLoadAsConfiguration` via the status bar.
    [[nodiscard]] std::size_t Replace(const std::vector<loglib::LogConfiguration::AnchorEntry> &entries);

    /// Anchor colour for @p key, or nullopt if not anchored.
    [[nodiscard]] std::optional<uint8_t> ColorFor(const Key &key) const noexcept;

    /// Snapshot for `LogConfiguration::anchors`, sorted by
    /// `(locator, lineId)` so saved JSON is byte-stable.
    ///
    /// Runtime-only anchors (empty locator) are dropped: their
    /// `lineId` is not stable across sessions, so persisting them
    /// would later collide with unrelated ids. Use
    /// `EntriesIncludingRuntimeOnly` for diagnostics.
    [[nodiscard]] std::vector<loglib::LogConfiguration::AnchorEntry> Entries() const;

    /// Like `Entries` but keeps runtime-only anchors. Diagnostics
    /// and tests only; save paths must use `Entries`.
    [[nodiscard]] std::vector<loglib::LogConfiguration::AnchorEntry> EntriesIncludingRuntimeOnly() const;

    [[nodiscard]] std::size_t Count() const noexcept;

    /// Cheap check for `LogModel::data()` to skip the anchor branch.
    [[nodiscard]] bool Empty() const noexcept;

signals:
    /// One key added, recoloured, or removed. Listeners can scope
    /// their repaint to the matching row(s).
    void anchorChanged(const AnchorManager::Key &key);

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

    std::unordered_map<Key, uint8_t, KeyHash> mAnchors;
};

// Required for `Qt::QueuedConnection` on `anchorChanged`: Qt copies
// the argument into the event queue and needs the type registered.
Q_DECLARE_METATYPE(AnchorManager::Key)
