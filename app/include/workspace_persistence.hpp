// SPDX-License-Identifier: MIT
//
// Task 8.1 / 8.2 -- grouped multi-window/tab workspace persistence.
//
// This module owns the "what did every open window look like at
// quit?" cross-launch state. Individual session snapshots (which
// row was pinned, which filters were live, which timezone was
// preferred) stay in `SessionHistoryManager`; this file adds a
// SIDECAR schema that captures the *grouping* -- ordered windows,
// each with an ordered tab strip and an active-tab pointer -- so
// launch-time restore reproduces the multi-tab / multi-window
// arrangement the user quit with. See PRD section on grouped
// workspace restoration and task 8's checklist for the FR list.
//
// Design points
// -------------
// - Versioned value types: `Workspace{}` carries `schemaVersion`
//   so future non-additive changes can gate reads on it. Today
//   there is exactly one version (`kSchemaVersion = 1`); an
//   older or newer read simply returns an empty workspace,
//   matching the "fail closed, do not loop" behaviour the
//   `SessionHistoryManager::TakeOpenWindowsAtQuit` sibling uses.
// - Bounded sizes: reads reject workspaces with more windows /
//   tabs / arbitrary strings than the constants below. The
//   ceilings are generous compared to human workflows (hundreds
//   of tabs) but small enough to bound worst-case IO / restore
//   time on a corrupted / adversarial value.
// - Atomic write: writes serialise to a `<path>.tmp` sibling and
//   `rename()` on top of the destination. On Windows the rename
//   still atomically replaces the target (Qt maps this onto
//   `MoveFileEx` with `MOVEFILE_REPLACE_EXISTING`).
// - Single authoritative read + take: `Read()` returns the
//   current workspace without clearing; `Take()` reads + wipes
//   under one lock acquisition so a mid-restore crash cannot
//   loop. Callers pick the semantic they need; there is no
//   third alternate schema reader (an anti-pattern the PRD
//   explicitly forbids).
// - Coexistence with `SessionHistoryManager`: the flat
//   `openWindowsAtQuit` list stays for backward compatibility;
//   the workspace file is a SUPERSET that additionally captures
//   ordering and per-tab structure. Removing the flat list is
//   deferred to task 8.14; today's restore prefers the workspace
//   file when present and falls back to the flat list otherwise.

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

/// Restore policy hint for a persisted tab. Restore paths
/// respect the source mode's natural constraints (network /
/// stdin cannot re-attach implicitly) regardless of this hint;
/// it exists to make user-authored `--restore=no` overrides
/// available and to let future revisions add per-tab opt-out
/// without a schema break.
enum class RestorePolicy : std::uint8_t
{
    /// Default: restore this tab when the parent window
    /// restores. File sources reopen; network / stdin surface
    /// as disconnected placeholders (tasks 8.9 / 8.10).
    Restore = 0,
    /// User has explicitly opted this tab out of restore. The
    /// tab is skipped entirely at launch, but its uuid is
    /// preserved in the workspace so a subsequent quit does
    /// not silently drop it.
    Skip = 1,
};

/// Source-mode descriptor stored per tab. Mirrors the seven
/// modes the PRD enumerates. Deliberately narrow (fits in one
/// byte) so the value type stays cheap to copy.
enum class SourceMode : std::uint8_t
{
    Empty = 0,        ///< Untitled / freshly-cleared tab (no bound source).
    File = 1,         ///< Static single-file open.
    MultiFile = 2,    ///< Multi-file open (queue drained).
    Compressed = 3,   ///< Compressed / decompressed archive open.
    Bundle = 4,       ///< Session bundle (embedded config + rows).
    LiveTailFile = 5, ///< File-tail live session.
    Network = 6,      ///< TCP / UDP listener.
    Stdin = 7,        ///< Standard input capture.
    ConfigOnly = 8,   ///< Configuration-only session (no rows).
};

/// One tab in the persisted workspace. `sessionUuid` matches
/// `SessionHistoryManager`'s recents index; a null uuid means
/// the tab was empty at quit and needs no restore work beyond
/// slot allocation.
struct WorkspaceTab
{
    QString sessionUuid;
    SourceMode sourceMode = SourceMode::Empty;
    RestorePolicy restorePolicy = RestorePolicy::Restore;
};

/// One window in the persisted workspace. `windowUuid` gives
/// each window a stable identity across launches (used by 8.11
/// MRU routing so an external open lands on the "same" window
/// the user last interacted with).
struct WorkspaceWindow
{
    QString windowUuid;
    QByteArray geometry;            ///< `QWidget::saveGeometry()` blob.
    QByteArray dockState;           ///< `QMainWindow::saveState()` blob.
    std::vector<WorkspaceTab> tabs; ///< Ordered tab strip.
    int activeTabIndex = 0;         ///< Which tab was current at quit.
};

/// Root workspace value. `schemaVersion` guards non-additive
/// changes; today's only accepted version is
/// `SCHEMA_VERSION`. `mruOrder` lists window uuids from most-
/// recently-focused to least; the primary consumes it during
/// external-open routing (task 8.11).
struct Workspace
{
    std::uint32_t schemaVersion = 0;
    std::vector<WorkspaceWindow> windows;
    QStringList mruOrder;
};

/// Static facade around the JSON-on-disk workspace snapshot.
///
/// Post-tabs review-round bug #6 note: an earlier version of this
/// docstring claimed the writer takes the same cross-process
/// mutex as `SessionHistoryManager` (`OpenWindowsMutex` /
/// `AcquireRecentsLock`). That was aspirational — the writer
/// intentionally does not acquire that lock. The workspace and
/// recents index live in the same directory but in DIFFERENT
/// files, and the two writers do not need combined atomicity to
/// stay coherent: `Write` serialises through `QSaveFile`'s
/// `rename()` so per-file atomicity is preserved, and the
/// `IsPublishingEnabled()` gate at the top of `Write` gives
/// peer-vs-primary isolation. The audit note in
/// `tasks/tasks-main-window-session-tabs.md §9.12` has been
/// updated to reflect the actual guarantee. If a future feature
/// requires atomic multi-file publish (e.g. tying a workspace
/// snapshot to a specific `recents.json` revision), promote
/// `AcquireRecentsLock` to a public `SessionHistoryManager`
/// static and wrap Read/Take/Write here at that point.
class WorkspacePersistence
{
public:
    /// Wire schema version. Bump on non-additive changes; a
    /// value mismatch on read yields an empty workspace.
    static constexpr std::uint32_t SCHEMA_VERSION = 1;

    /// Per-launch bounds on the read side. Values above these
    /// ceilings are treated as corrupted input; the read
    /// returns an empty workspace. Chosen generously (multi-
    /// hundred tab strips are plausible power-user workflows)
    /// but bounded so a hostile / truncated file cannot make
    /// restore hang.
    static constexpr std::size_t MAX_WINDOWS = 64;
    static constexpr std::size_t MAX_TABS_PER_WINDOW = 512;
    static constexpr std::size_t MAX_UUID_LENGTH = 64;
    static constexpr std::size_t MAX_GEOMETRY_BYTES = 64 * 1024;
    static constexpr std::size_t MAX_DOCK_STATE_BYTES = 128 * 1024;

    /// Read the workspace WITHOUT wiping. Returns an empty
    /// workspace on any of: missing file, schema mismatch,
    /// bound violation, JSON parse failure, or lock timeout.
    /// Callers use this for peek-only diagnostic paths; the
    /// restore fan uses `Take()` instead.
    [[nodiscard]] static Workspace Read();

    /// Atomic read-and-wipe. Same failure modes as `Read()`;
    /// the wipe is skipped when the read failed. A successful
    /// take leaves the on-disk file present but empty so an
    /// interrupted restore does not loop.
    [[nodiscard]] static Workspace Take();

    /// Serialise + atomically write the workspace. Fails
    /// closed (returns false, does not throw) on any of:
    /// publish disabled, lock timeout, sanitize-time bound
    /// violation, IO failure. `sanitize` clamps
    /// oversize / malformed entries in place before writing so
    /// the on-disk file always parses.
    static bool Write(Workspace workspace);

    /// True when at least one hosted window / tab is present
    /// on the current on-disk workspace. Cheap peek used by
    /// startup restore-vs-splash gating.
    [[nodiscard]] static bool HasPersistedWorkspace();

    /// Wipe the on-disk workspace immediately (unlike `Take()`
    /// which requires a caller-side read consumer). Used by
    /// the "Restore last session on launch" preference toggle
    /// and the test fixture teardown.
    static void Clear();

    /// Resolve the workspace JSON path under the shared
    /// application-data directory. Exposed for test fixtures
    /// that need to seed / assert the on-disk state.
    [[nodiscard]] static QString WorkspaceFilePath();

    /// Per-user directory hosting the workspace file and the
    /// existing `SessionHistoryManager` state. Public so tests
    /// can override the directory via `QStandardPaths` before
    /// invoking `Read` / `Write`.
    [[nodiscard]] static QDir DefaultWorkspaceDir();

    /// Clamp / drop entries that exceed the bounded-size
    /// invariants. Exposed for tests so an oversize workspace
    /// can be constructed and its sanitized shape asserted
    /// against the schema.
    static void Sanitize(Workspace &workspace);
};

} // namespace slv::persistence
