#pragma once

#include <cstddef>

/// Persistence facade for the **Streaming** and **Static (file mode)**
/// display-order groups of `PreferencesEditor`. Ok/Cancel transactional:
/// the editor mutates in-memory state; `SaveConfiguration` commits to
/// `QSettings`, `LoadConfiguration` reverts to the on-disk values. Tests
/// touch the same `QSettings` keys directly.
class StreamingControl
{
public:
    /// Default retention cap, also the fallback used by
    /// `LogModel::BeginStreaming(StreamLineSource, ...)` when no value
    /// has been set elsewhere.
    static constexpr size_t DEFAULT_RETENTION_LINES = 10'000;

    /// Allowed retention range surfaced by the spinbox.
    static constexpr size_t MIN_RETENTION_LINES = 1'000;
    static constexpr size_t MAX_RETENTION_LINES = 1'000'000;

    /// Default for **Show newest lines first** (both stream and static
    /// modes): off, so the existing append-at-bottom behaviour is
    /// preserved for users who do not opt in.
    static constexpr bool DEFAULT_NEWEST_FIRST = false;

    /// Commit the in-memory configuration to `QSettings`.
    static void SaveConfiguration();

    /// Reload from `QSettings`, falling back to defaults when nothing
    /// has been persisted.
    static void LoadConfiguration();

    static size_t RetentionLines();

    /// Set the in-memory cap; call `SaveConfiguration` to persist.
    static void SetRetentionLines(size_t value);

    /// Newest-first display order for **stream mode** sessions
    /// (live tail). Backed by the `streaming/newestFirst` setting.
    static bool IsNewestFirst();

    /// Set the in-memory flag; call `SaveConfiguration` to persist.
    static void SetNewestFirst(bool value);

    /// Newest-first display order for **static (file mode)** sessions.
    /// Backed by the `static/newestFirst` setting; independent of the
    /// stream-mode flag so users can pick a different default per mode.
    static bool IsStaticNewestFirst();

    /// Set the in-memory flag; call `SaveConfiguration` to persist.
    static void SetStaticNewestFirst(bool value);

private:
    struct Configuration
    {
        size_t retentionLines = DEFAULT_RETENTION_LINES;
        /// Stream-mode newest-first flag. Persisted at `streaming/newestFirst`.
        bool newestFirst = DEFAULT_NEWEST_FIRST;
        /// Static-mode newest-first flag. Persisted at `static/newestFirst`.
        bool staticNewestFirst = DEFAULT_NEWEST_FIRST;
    };

    static Configuration mConfiguration;
};
