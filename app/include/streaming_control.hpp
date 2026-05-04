#pragma once

#include <cstddef>

/// Persistence facade for the **Streaming** group of `PreferencesEditor`.
/// Ok/Cancel transactional: the editor mutates in-memory state;
/// `SaveConfiguration` commits to `QSettings`, `LoadConfiguration`
/// reverts to the on-disk values. Tests touch the same `QSettings`
/// keys directly.
class StreamingControl
{
public:
    /// Default retention cap, also the fallback used by
    /// `LogModel::BeginStreaming(StreamLineSource, ...)` when no value
    /// has been set elsewhere.
    static constexpr size_t kDefaultRetentionLines = 10'000;

    /// Allowed retention range surfaced by the spinbox.
    static constexpr size_t kMinRetentionLines = 1'000;
    static constexpr size_t kMaxRetentionLines = 1'000'000;

    /// Default for **Show newest lines first**: off, so the existing
    /// append-at-bottom behaviour is preserved for users who do not
    /// opt in.
    static constexpr bool kDefaultNewestFirst = false;

    /// Commit the in-memory configuration to `QSettings`.
    static void SaveConfiguration();

    /// Reload from `QSettings`, falling back to defaults when nothing
    /// has been persisted.
    static void LoadConfiguration();

    static size_t RetentionLines();

    /// Set the in-memory cap; call `SaveConfiguration` to persist.
    static void SetRetentionLines(size_t value);

    static bool IsNewestFirst();

    /// Set the in-memory flag; call `SaveConfiguration` to persist.
    static void SetNewestFirst(bool value);

private:
    struct Configuration
    {
        size_t retentionLines = kDefaultRetentionLines;
        bool newestFirst = kDefaultNewestFirst;
    };

    static Configuration mConfiguration;
};
