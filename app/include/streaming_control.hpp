#pragma once

#include <cstddef>

/// Persistence facade for the **Streaming** group of `PreferencesEditor`.
/// Mirrors `AppearanceControl`'s Ok/Cancel transactional pattern (PRD §6
/// *Preferences*, OQ-10): the preferences editor mutates the live config
/// in-memory; `SaveConfiguration` (Ok) commits to `QSettings`,
/// `LoadConfiguration` (Cancel / startup) reverts to the on-disk value.
///
/// A separate class from `AppearanceControl` so a Cancel-from-appearance
/// does not also revert a streaming-retention change made on the same
/// dialog session. Tests touch the same `QSettings` keys directly.
class StreamingControl
{
public:
    /// Default retention cap surfaced in the Preferences spinbox. Mirrors
    /// `LogModel::kDefaultRetentionLines` (PRD 4.5.2).
    static constexpr size_t kDefaultRetentionLines = 10'000;

    /// Allowed retention range surfaced by the spinbox. Mirrors PRD 4.5.2
    /// (`1 000 .. 1 000 000`).
    static constexpr size_t kMinRetentionLines = 1'000;
    static constexpr size_t kMaxRetentionLines = 1'000'000;

    /// Default for the **Show newest lines first** stream toggle.
    /// Off by default so the existing append-at-bottom behaviour is
    /// preserved for users who do not opt in.
    static constexpr bool kDefaultNewestFirst = false;

    /// Persist the in-memory configuration to `QSettings` under
    /// `streaming/retentionLines` and `streaming/newestFirst` (Ok
    /// handler).
    static void SaveConfiguration();

    /// Reload the in-memory configuration from `QSettings` (Cancel handler /
    /// startup). Falls back to `kDefaultRetentionLines` /
    /// `kDefaultNewestFirst` when no value has been persisted yet.
    static void LoadConfiguration();

    /// In-memory retention cap. Mutated by the spinbox while the
    /// preferences dialog is open; committed to / reverted from
    /// `QSettings` by Save / LoadConfiguration.
    static size_t RetentionLines();

    /// Update the in-memory retention cap. Does not persist to `QSettings`
    /// — call `SaveConfiguration` from the Ok handler.
    static void SetRetentionLines(size_t value);

    /// In-memory **Show newest lines first** flag. Mutated by the
    /// preferences-dialog checkbox; committed to / reverted from
    /// `QSettings` by Save / LoadConfiguration.
    static bool IsNewestFirst();

    /// Update the in-memory newest-first flag. Does not persist to
    /// `QSettings` — call `SaveConfiguration` from the Ok handler.
    static void SetNewestFirst(bool value);

private:
    struct Configuration
    {
        size_t retentionLines = kDefaultRetentionLines;
        bool newestFirst = kDefaultNewestFirst;
    };

    static Configuration mConfiguration;
};
