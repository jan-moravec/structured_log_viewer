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
 * standard-input entries allocate an empty tab when restored. `label` stores
 * the automatic session name so those placeholders keep a useful title.
 * `customLabel` stores a user-assigned title and overrides `label` when set.
 */
struct WorkspaceTab
{
    /** @brief Saved-session UUID, or an empty string when no session can be reopened. */
    QString sessionUuid;
    /**
     * @brief Automatic display label captured at save time.
     *
     * Matches `SessionHistoryManager::BuildLabel` for a bound source
     * (file basename, `name + N more`, or a stdin / network locator).
     * Restored placeholder tabs use this when no source is rebound.
     */
    QString label;
    /**
     * @brief User-assigned tab title captured at save time.
     *
     * When non-empty, this overrides automatic naming after restore.
     */
    QString customLabel;
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
    /** @brief Generation identifier shared by this manifest and its snapshots. */
    std::uint64_t generation = 0;
    /** @brief Windows in persisted order. */
    std::vector<WorkspaceWindow> windows;
    /** @brief Window UUIDs ordered from most to least recently focused. */
    QStringList mruOrder;
};

/**
 * @brief Reads and writes the bounded JSON workspace snapshot.
 *
 * Session snapshots are written under `generations/<id>/` before the
 * workspace manifest is published. `Write()` uses `QSaveFile` so a failed
 * manifest commit leaves the previous complete generation in place.
 * Only `Write()` and `Publish()` observe the session-history publishing gate.
 */
class WorkspacePersistence
{
public:
    /**
     * @brief Schema version accepted by `Read()` and emitted by `Write()`.
     *
     * Any other version is treated as absent and left untouched on disk.
     */
    static constexpr std::uint32_t SCHEMA_VERSION = 1;

    /** @brief Maximum number of persisted windows and MRU entries. */
    static constexpr std::size_t MAX_WINDOWS = 64;
    /** @brief Maximum number of tabs persisted in one window. */
    static constexpr std::size_t MAX_TABS_PER_WINDOW = 512;
    /** @brief Maximum UTF-16 code-unit count for persisted UUID strings. */
    static constexpr std::size_t MAX_UUID_LENGTH = 64;
    /** @brief Maximum UTF-16 code-unit count for persisted tab labels. */
    static constexpr std::size_t MAX_TAB_LABEL_LENGTH = 256;
    /** @brief Maximum decoded size of one saved geometry blob. */
    static constexpr std::size_t MAX_GEOMETRY_BYTES = 64 * 1024;
    /** @brief Maximum decoded size of one saved dock-state blob. */
    static constexpr std::size_t MAX_DOCK_STATE_BYTES = 128 * 1024;
    /** @brief Maximum number of windows restored on one launch. */
    static constexpr std::size_t DEFAULT_RESTORE_CAP = 25;
    /** @brief Maximum workspace manifest size accepted by `Read()`. */
    static constexpr std::size_t MAX_WORKSPACE_FILE_BYTES = 32U * 1024U * 1024U;

    /**
     * @brief Launch restore prefix and the windows held back by the restore cap.
     */
    struct RestorePlan
    {
        /** @brief Windows restored on this launch, in persisted order. */
        Workspace toRestore;
        /** @brief Windows not restored on this launch, in persisted order. */
        Workspace deferred;
    };

    /**
     * @brief Reads the current workspace without modifying the file.
     *
     * Files larger than `MAX_WORKSPACE_FILE_BYTES` are rejected before the
     * contents are loaded. Invalid JSON, a schema mismatch, excess window,
     * tab, or MRU counts, and encoded state strings beyond their predecode
     * caps produce an empty workspace. UUID strings are truncated; decoded
     * state blobs beyond their byte bounds are dropped.
     *
     * @return The decoded workspace, or an empty workspace when the file is
     * missing, unreadable, oversized, or invalid.
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
     * @brief Removes the workspace file and generation directories.
     *
     * Does not consult the publishing gate.
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

    /**
     * @brief Splits @p workspace into a restore prefix and a deferred remainder.
     *
     * When the window count is within @p restoreCap, `toRestore` receives the
     * complete workspace and `deferred` is empty. Otherwise `toRestore` keeps
     * the first @p restoreCap windows and `deferred` keeps the rest. MRU order
     * is copied onto both sides.
     *
     * @param workspace Workspace to split; consumed.
     * @param restoreCap Maximum number of windows to restore on this launch.
     * @return The restore prefix and any deferred remainder.
     */
    [[nodiscard]] static RestorePlan PlanRestore(Workspace workspace, std::size_t restoreCap);

    /**
     * @brief Appends deferred windows that are not already present in @p captured.
     *
     * Captured windows keep their order. Deferred windows whose `windowUuid`
     * already appears in @p captured are skipped. MRU entries from @p captured
     * stay first; remaining deferred MRU entries are appended.
     *
     * @param captured Windows captured from currently live windows.
     * @param deferred Windows held back by a prior restore cap.
     * @return The merged workspace.
     */
    [[nodiscard]] static Workspace MergeCapturedWithDeferred(Workspace captured, const Workspace &deferred);

    /**
     * @brief Stores deferred windows for later merge on a normal quit.
     * @param deferred Windows held back by this launch's restore cap.
     */
    static void SetDeferredWindows(Workspace deferred);

    /**
     * @brief Takes the deferred windows stored by `SetDeferredWindows`.
     *
     * The stored slot is cleared. Used by a normal quit before
     * `MergeCapturedWithDeferred`.
     *
     * @return The deferred workspace, or an empty workspace when none is stored.
     */
    [[nodiscard]] static Workspace TakeDeferredWindows();

    /**
     * @brief Reads the workspace and prepares the current launch's restore plan.
     *
     * The published generation remains on disk. When windows remain deferred,
     * `SetDeferredWindows` stores the remainder for a later quit merge.
     *
     * @param restoreCap Maximum number of windows to restore on this launch.
     * @return The restore prefix and any deferred remainder.
     */
    [[nodiscard]] static RestorePlan LoadForLaunch(std::size_t restoreCap);

    /**
     * @brief Returns the directory that stores session snapshots for one generation.
     *
     * @param generation Generation identifier; zero yields an empty path.
     * @return Absolute path to `generations/<generation>/`, or empty when @p generation is zero.
     */
    [[nodiscard]] static QString GenerationDirPath(std::uint64_t generation);

    /**
     * @brief Returns the session JSON path inside a generation directory.
     *
     * @param generation Generation identifier.
     * @param sessionUuid Session UUID stem.
     * @return Snapshot path, or empty when the UUID is not well-formed.
     */
    [[nodiscard]] static QString SessionSnapshotPath(std::uint64_t generation, const QString &sessionUuid);

    /**
     * @brief Copies referenced session snapshots into a generation directory.
     *
     * Each tab's snapshot is copied from the recents sessions directory, then
     * from the currently published generation. Tabs whose snapshot cannot be
     * copied have `sessionUuid` cleared so the published manifest does not
     * claim they are restorable.
     *
     * @param generation Destination generation identifier; must be nonzero.
     * @param workspace Workspace whose tab UUIDs are copied; updated in place.
     * @return `true` when the generation directory exists or was created.
     */
    static bool WriteGenerationSnapshots(std::uint64_t generation, Workspace &workspace);

    /**
     * @brief Deletes generation directories other than @p keepGeneration.
     *
     * @param keepGeneration Generation to retain; zero removes every generation directory.
     */
    static void CollectSupersededGenerations(std::uint64_t keepGeneration);

    /**
     * @brief Writes snapshots for a new generation, publishes the manifest, then collects old generations.
     *
     * The previous complete generation remains until the manifest commit
     * succeeds. A failed snapshot copy clears that tab's UUID rather than
     * publishing a false restore claim.
     *
     * @param workspace Workspace to publish; generation is assigned on success.
     * @return `true` when the manifest commit succeeds.
     */
    static bool Publish(Workspace workspace);
};

} // namespace slv::persistence
