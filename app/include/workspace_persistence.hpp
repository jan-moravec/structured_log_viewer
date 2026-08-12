// SPDX-License-Identifier: MIT

#pragma once

#include <QDir>
#include <QString>
#include <QStringList>
#include <QUuid>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace slv::persistence
{

/**
 * @brief Controls whether a persisted tab is considered during workspace restoration.
 */
enum class RestorePolicy : std::uint8_t
{
    /** @brief Allocate the tab and restore its source when the source is reopenable. */
    Restore = 0,
    /** @brief Allocate the tab without reopening its persisted source. */
    Skip = 1,
};

/**
 * @brief Identifies the source mode associated with a persisted tab.
 */
enum class SourceMode : std::uint8_t
{
    /** @brief Untitled tab with no bound source. */
    Empty = 0,
    /** @brief Static single-file source. */
    File = 1,
    /** @brief Multi-file source. */
    MultiFile = 2,
    /** @brief Compressed-file source. */
    Compressed = 3,
    /** @brief Session-bundle source. */
    Bundle = 4,
    /** @brief Live-tailed file source. */
    LiveTailFile = 5,
    /** @brief TCP or UDP listener source. */
    Network = 6,
    /** @brief Standard-input source. */
    Stdin = 7,
    /** @brief Configuration-only session with no rows. */
    ConfigOnly = 8,
};

/**
 * @brief Describes one tab in a persisted workspace.
 *
 * `sessionUuid` identifies a saved session when one exists. Empty, network, and
 * standard-input entries allocate an empty tab when restored.
 */
struct WorkspaceTab
{
    /** @brief Saved-session UUID, or an empty string when no session can be reopened. */
    QString sessionUuid;
    /** @brief Source mode captured for this tab. */
    SourceMode sourceMode = SourceMode::Empty;
    /** @brief Restore policy captured for this tab. */
    RestorePolicy restorePolicy = RestorePolicy::Restore;
};

/**
 * @brief Describes one window, its ordered tabs, and its saved Qt window state.
 */
struct WorkspaceWindow
{
    /** @brief Stable UUID used to identify the window across launches. */
    QString windowUuid;
    /** @brief Blob returned by `QWidget::saveGeometry()`. */
    QByteArray geometry;
    /** @brief Blob returned by `QMainWindow::saveState()`. */
    QByteArray dockState;
    /** @brief Tabs in visual strip order. */
    std::vector<WorkspaceTab> tabs;
    /** @brief Index of the tab that was active when captured. */
    int activeTabIndex = 0;
};

/**
 * @brief Stores the versioned, ordered multi-window workspace snapshot.
 *
 * `mruOrder` lists window UUIDs from most recently focused to least recently
 * focused.
 */
struct Workspace
{
    /** @brief Serialized schema version. */
    std::uint32_t schemaVersion = 0;
    /** @brief Windows in persisted order. */
    std::vector<WorkspaceWindow> windows;
    /** @brief Window UUIDs ordered from most to least recently focused. */
    QStringList mruOrder;
};

/**
 * @brief Reads and writes the bounded JSON workspace snapshot.
 *
 * `Write()` uses `QSaveFile`, providing atomic replacement of the workspace
 * file itself. Workspace and session-history files are not published as one
 * transaction. Only `Write()` observes the session-history publishing gate.
 */
class WorkspacePersistence
{
public:
    /** @brief Schema version accepted by `Read()` and emitted by `Write()`. */
    static constexpr std::uint32_t SCHEMA_VERSION = 1;

    /** @brief Maximum number of persisted windows and MRU entries. */
    static constexpr std::size_t MAX_WINDOWS = 64;
    /** @brief Maximum number of tabs persisted in one window. */
    static constexpr std::size_t MAX_TABS_PER_WINDOW = 512;
    /** @brief Maximum UTF-16 code-unit count for persisted UUID strings. */
    static constexpr std::size_t MAX_UUID_LENGTH = 64;
    /** @brief Maximum decoded size of one saved geometry blob. */
    static constexpr std::size_t MAX_GEOMETRY_BYTES = 64 * 1024;
    /** @brief Maximum decoded size of one saved dock-state blob. */
    static constexpr std::size_t MAX_DOCK_STATE_BYTES = 128 * 1024;

    /**
     * @brief Reads the current workspace without modifying the file.
     *
     * Invalid JSON, a schema mismatch, excess window, tab, or MRU counts, and
     * encoded state strings beyond their predecode caps produce an empty
     * workspace. UUID strings are truncated; decoded state blobs beyond their
     * byte bounds are dropped.
     *
     * @return The decoded workspace, or an empty workspace when the file is
     * missing, unreadable, or invalid.
     */
    [[nodiscard]] static Workspace Read();

    /**
     * @brief Reads the workspace and then attempts to replace it with an empty workspace.
     *
     * The empty replacement is attempted only when the decoded workspace contains
     * windows or MRU entries. The read and wipe are not one transaction. A wipe
     * failure is logged and does not change the returned workspace.
     *
     * @return The same decoded value that `Read()` would return.
     */
    [[nodiscard]] static Workspace Take();

    /**
     * @brief Sanitizes and atomically replaces the workspace file.
     *
     * Sanitization truncates excess windows, tabs, UUIDs, and MRU entries,
     * drops oversized geometry and dock-state blobs, and normalizes invalid
     * active-tab indices. The write is rejected when publishing is disabled.
     *
     * @param workspace Workspace value to sanitize and serialize.
     * @return `true` when `QSaveFile::commit()` succeeds; otherwise `false`.
     */
    static bool Write(Workspace workspace);

    /**
     * @brief Checks whether the decoded workspace contains windows or MRU entries.
     * @return `true` when `Read()` returns either collection as non-empty.
     */
    [[nodiscard]] static bool HasPersistedWorkspace();

    /**
     * @brief Removes the workspace file without consulting the publishing gate.
     */
    static void Clear();

    /**
     * @brief Resolves the workspace JSON path in the application-data directory.
     * @return Absolute or platform-native path to `workspace.json`.
     */
    [[nodiscard]] static QString WorkspaceFilePath();

    /**
     * @brief Returns the per-user directory shared with session history.
     * @return Directory containing workspace and session-history state.
     */
    [[nodiscard]] static QDir DefaultWorkspaceDir();

    /**
     * @brief Normalizes a workspace to the write-side bounds and schema version.
     * @param workspace Workspace to modify in place.
     */
    static void Sanitize(Workspace &workspace);
};

} // namespace slv::persistence
