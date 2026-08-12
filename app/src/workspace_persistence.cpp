// SPDX-License-Identifier: MIT

#include "workspace_persistence.hpp"

#include "session_history_manager.hpp"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringConverter>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace slv::persistence
{
namespace
{

constexpr const char *WORKSPACE_FILE_NAME = "workspace.json";

constexpr const char *KEY_SCHEMA_VERSION = "schemaVersion";
constexpr const char *KEY_WINDOWS = "windows";
constexpr const char *KEY_MRU_ORDER = "mruOrder";
constexpr const char *KEY_WINDOW_UUID = "windowUuid";
constexpr const char *KEY_GEOMETRY = "geometry";
constexpr const char *KEY_DOCK_STATE = "dockState";
constexpr const char *KEY_TABS = "tabs";
constexpr const char *KEY_ACTIVE_TAB_INDEX = "activeTabIndex";
constexpr const char *KEY_SESSION_UUID = "sessionUuid";
constexpr const char *KEY_SOURCE_MODE = "sourceMode";
constexpr const char *KEY_RESTORE_POLICY = "restorePolicy";

/// Bounded string clamp: values longer than @p maxLen are
/// truncated. Empty / null strings pass through unchanged so
/// the sanitize step does not manufacture non-empty content.
QString ClampString(const QString &value, std::size_t maxLen)
{
    if (std::cmp_less_equal(value.size(), maxLen))
    {
        return value;
    }
    return value.left(static_cast<int>(maxLen));
}

/// Bounded byte-array clamp: values longer than @p maxLen are
/// dropped (returned empty). Truncating a `QMainWindow::saveState`
/// blob would produce garbage on restore; drop the whole blob so
/// restore falls back to the default layout instead.
QByteArray ClampBytes(const QByteArray &value, std::size_t maxLen)
{
    if (std::cmp_less_equal(value.size(), maxLen))
    {
        return value;
    }
    return {};
}

QJsonValue ToJson(const WorkspaceTab &tab)
{
    QJsonObject obj;
    obj.insert(QLatin1String(KEY_SESSION_UUID), tab.sessionUuid);
    obj.insert(QLatin1String(KEY_SOURCE_MODE), static_cast<int>(tab.sourceMode));
    obj.insert(QLatin1String(KEY_RESTORE_POLICY), static_cast<int>(tab.restorePolicy));
    return obj;
}

QJsonValue ToJson(const WorkspaceWindow &window)
{
    QJsonObject obj;
    obj.insert(QLatin1String(KEY_WINDOW_UUID), window.windowUuid);
    obj.insert(QLatin1String(KEY_GEOMETRY), QString::fromLatin1(window.geometry.toBase64()));
    obj.insert(QLatin1String(KEY_DOCK_STATE), QString::fromLatin1(window.dockState.toBase64()));
    obj.insert(QLatin1String(KEY_ACTIVE_TAB_INDEX), window.activeTabIndex);
    QJsonArray tabsArr;
    for (const auto &tab : window.tabs)
    {
        tabsArr.append(ToJson(tab));
    }
    obj.insert(QLatin1String(KEY_TABS), tabsArr);
    return obj;
}

QJsonDocument BuildDoc(const Workspace &workspace)
{
    QJsonObject root;
    root.insert(QLatin1String(KEY_SCHEMA_VERSION), static_cast<int>(workspace.schemaVersion));
    QJsonArray windowsArr;
    for (const auto &window : workspace.windows)
    {
        windowsArr.append(ToJson(window));
    }
    root.insert(QLatin1String(KEY_WINDOWS), windowsArr);
    QJsonArray mruArr;
    for (const QString &uuid : workspace.mruOrder)
    {
        mruArr.append(uuid);
    }
    root.insert(QLatin1String(KEY_MRU_ORDER), mruArr);
    return QJsonDocument{root};
}

WorkspaceTab TabFromJson(const QJsonObject &obj)
{
    WorkspaceTab tab;
    tab.sessionUuid =
        ClampString(obj.value(QLatin1String(KEY_SESSION_UUID)).toString(), WorkspacePersistence::MAX_UUID_LENGTH);
    const int modeInt = obj.value(QLatin1String(KEY_SOURCE_MODE)).toInt(static_cast<int>(SourceMode::Empty));
    if (modeInt < 0 || modeInt > static_cast<int>(SourceMode::ConfigOnly))
    {
        tab.sourceMode = SourceMode::Empty;
    }
    else
    {
        tab.sourceMode = static_cast<SourceMode>(modeInt);
    }
    const int policyInt = obj.value(QLatin1String(KEY_RESTORE_POLICY)).toInt(static_cast<int>(RestorePolicy::Restore));
    tab.restorePolicy =
        (policyInt == static_cast<int>(RestorePolicy::Skip)) ? RestorePolicy::Skip : RestorePolicy::Restore;
    return tab;
}

// Post-tabs review-round bug #M3: signal "this window is
// bounds-violating, drop it" without introducing an
// `std::optional` at every call site. `windowUuid.isNull()`
// (as opposed to `isEmpty()`) is the sentinel because a
// legitimate captured window uuid is always assigned (empty
// or non-empty QString), never default-constructed to null.
[[nodiscard]] inline WorkspaceWindow InvalidWindow()
{
    WorkspaceWindow w;
    w.windowUuid = QString(); // isNull() == true
    return w;
}

[[nodiscard]] inline bool IsInvalidWindow(const WorkspaceWindow &w) noexcept
{
    return w.windowUuid.isNull();
}

WorkspaceWindow WindowFromJson(const QJsonObject &obj)
{
    WorkspaceWindow window;
    // Post-tabs review-round bug #M3: force `windowUuid` to
    // non-null (empty string, but assigned) so the sentinel
    // above works. Read + clamp then follows.
    window.windowUuid = QString(QLatin1String(""));
    window.windowUuid =
        ClampString(obj.value(QLatin1String(KEY_WINDOW_UUID)).toString(), WorkspacePersistence::MAX_UUID_LENGTH);
    // Post-tabs review-round bug #M3: reject oversize base64
    // BEFORE decoding. A hostile / corrupted string of length
    // N > 4/3 * MAX would decode into an N * 3/4 byte array
    // before `ClampBytes` gets a chance to trim it -- an easy
    // OOM DoS on the read side. Base64 encodes 3 input bytes
    // as 4 output chars, so clamp the string length to
    // `(MAX * 4 + 2) / 3` (round up to include padding).
    const auto maxGeoB64Chars = static_cast<qsizetype>((WorkspacePersistence::MAX_GEOMETRY_BYTES * 4 + 2) / 3 + 4);
    const auto maxDockB64Chars = static_cast<qsizetype>((WorkspacePersistence::MAX_DOCK_STATE_BYTES * 4 + 2) / 3 + 4);
    const QString geoStr = obj.value(QLatin1String(KEY_GEOMETRY)).toString();
    const QString dockStr = obj.value(QLatin1String(KEY_DOCK_STATE)).toString();
    if (geoStr.size() > maxGeoB64Chars || dockStr.size() > maxDockB64Chars)
    {
        // Fail-closed per the header contract: an over-cap
        // blob is treated as corrupted input, and the whole
        // window is dropped rather than silently trimmed to
        // a broken geometry / dock state.
        return InvalidWindow();
    }
    const QByteArray geoB64 = geoStr.toLatin1();
    window.geometry = ClampBytes(QByteArray::fromBase64(geoB64), WorkspacePersistence::MAX_GEOMETRY_BYTES);
    const QByteArray dockB64 = dockStr.toLatin1();
    window.dockState = ClampBytes(QByteArray::fromBase64(dockB64), WorkspacePersistence::MAX_DOCK_STATE_BYTES);
    window.activeTabIndex = obj.value(QLatin1String(KEY_ACTIVE_TAB_INDEX)).toInt(0);
    const QJsonArray tabsArr = obj.value(QLatin1String(KEY_TABS)).toArray();
    // Post-tabs review-round bug #M3: fail-closed on
    // over-cap tab counts per the header contract. Silent
    // truncation combined with `Take()`'s atomic wipe would
    // permanently lose the dropped tabs; better to reject the
    // whole window and preserve the file for user inspection.
    if (std::cmp_greater(tabsArr.size(), WorkspacePersistence::MAX_TABS_PER_WINDOW))
    {
        return InvalidWindow();
    }
    // Post-tabs review-round bug #M3: reserve from the
    // POST-clamp bound so a corrupted `tabsArr.size()`
    // can never OOM-DoS the reader.
    window.tabs.reserve(std::min(static_cast<std::size_t>(tabsArr.size()), WorkspacePersistence::MAX_TABS_PER_WINDOW));
    for (int i = 0; i < tabsArr.size() && window.tabs.size() < WorkspacePersistence::MAX_TABS_PER_WINDOW; ++i)
    {
        if (!tabsArr.at(i).isObject())
        {
            continue;
        }
        window.tabs.push_back(TabFromJson(tabsArr.at(i).toObject()));
    }
    if (window.activeTabIndex < 0 || std::cmp_greater_equal(window.activeTabIndex, window.tabs.size()))
    {
        // Both branches intentionally collapse to 0: an empty
        // tab strip has no valid active index and a captured
        // out-of-range index falls back to the first tab. Kept
        // as a single assignment so the branch-clone check
        // stays quiet.
        window.activeTabIndex = 0;
    }
    return window;
}

Workspace WorkspaceFromDoc(const QJsonDocument &doc)
{
    Workspace workspace;
    if (!doc.isObject())
    {
        return workspace;
    }
    const QJsonObject root = doc.object();
    const std::uint32_t version = static_cast<std::uint32_t>(root.value(QLatin1String(KEY_SCHEMA_VERSION)).toInt(0));
    if (version != WorkspacePersistence::SCHEMA_VERSION)
    {
        // Schema mismatch: fail closed. Older / newer files are
        // treated as absent; the on-disk state is preserved for
        // manual inspection but restore proceeds with an empty
        // workspace. Matches the `TakeOpenWindowsAtQuit` fail-
        // closed contract.
        return workspace;
    }
    workspace.schemaVersion = version;
    const QJsonArray windowsArr = root.value(QLatin1String(KEY_WINDOWS)).toArray();
    // Post-tabs review-round bug #M3: fail-closed on over-cap
    // window / MRU counts (contract in
    // `workspace_persistence.hpp` MAX_WINDOWS docstring). Silent
    // truncation would combine with `Take()`'s atomic wipe to
    // permanently lose the surplus.
    if (std::cmp_greater(windowsArr.size(), WorkspacePersistence::MAX_WINDOWS))
    {
        return Workspace{};
    }
    workspace.windows.reserve(std::min(static_cast<std::size_t>(windowsArr.size()), WorkspacePersistence::MAX_WINDOWS));
    for (int i = 0; i < windowsArr.size() && workspace.windows.size() < WorkspacePersistence::MAX_WINDOWS; ++i)
    {
        if (!windowsArr.at(i).isObject())
        {
            continue;
        }
        WorkspaceWindow window = WindowFromJson(windowsArr.at(i).toObject());
        if (IsInvalidWindow(window))
        {
            // Post-tabs review-round bug #M3: one over-cap
            // window fails the whole read (same fail-closed
            // rationale as the top-level cap). Any single-
            // window over-cap almost certainly means the writer
            // is corrupt, not that this one window is malicious.
            return Workspace{};
        }
        workspace.windows.push_back(std::move(window));
    }
    const QJsonArray mruArr = root.value(QLatin1String(KEY_MRU_ORDER)).toArray();
    if (std::cmp_greater(mruArr.size(), WorkspacePersistence::MAX_WINDOWS))
    {
        return Workspace{};
    }
    for (int i = 0;
         i < mruArr.size() && workspace.mruOrder.size() < static_cast<int>(WorkspacePersistence::MAX_WINDOWS);
         ++i)
    {
        const QString uuid = ClampString(mruArr.at(i).toString(), WorkspacePersistence::MAX_UUID_LENGTH);
        if (!uuid.isEmpty())
        {
            workspace.mruOrder.append(uuid);
        }
    }
    return workspace;
}

} // namespace

QDir WorkspacePersistence::DefaultWorkspaceDir()
{
    // Share the directory with `SessionHistoryManager` so both
    // files (recents index + workspace snapshot) live under
    // `AppDataLocation` and are removed together on uninstall.
    //
    // Post-tabs review-round bug #6 note: an earlier comment
    // claimed this co-location implied that `AcquireRecentsLock`
    // (private to `session_history_manager.cpp`) also covers
    // this file's writes. It does not -- the workspace writer
    // does not acquire that lock. Per-file atomicity via
    // `QSaveFile` in `Write()` is sufficient today because the
    // two writers publish independent data; there is no
    // multi-file invariant that would need combined atomicity.
    return SessionHistoryManager::DefaultSessionsDir();
}

QString WorkspacePersistence::WorkspaceFilePath()
{
    return DefaultWorkspaceDir().filePath(QLatin1String(WORKSPACE_FILE_NAME));
}

void WorkspacePersistence::Sanitize(Workspace &workspace)
{
    workspace.schemaVersion = SCHEMA_VERSION;
    if (workspace.windows.size() > MAX_WINDOWS)
    {
        workspace.windows.resize(MAX_WINDOWS);
    }
    for (auto &window : workspace.windows)
    {
        window.windowUuid = ClampString(window.windowUuid, MAX_UUID_LENGTH);
        window.geometry = ClampBytes(window.geometry, MAX_GEOMETRY_BYTES);
        window.dockState = ClampBytes(window.dockState, MAX_DOCK_STATE_BYTES);
        if (window.tabs.size() > MAX_TABS_PER_WINDOW)
        {
            window.tabs.resize(MAX_TABS_PER_WINDOW);
        }
        for (auto &tab : window.tabs)
        {
            tab.sessionUuid = ClampString(tab.sessionUuid, MAX_UUID_LENGTH);
        }
        if (window.activeTabIndex < 0 || std::cmp_greater_equal(window.activeTabIndex, window.tabs.size()))
        {
            // Both branches collapse to 0: an empty tab strip
            // has no valid active index and an out-of-range
            // saved index falls back to the first tab.
            window.activeTabIndex = 0;
        }
    }
    QStringList clampedMru;
    clampedMru.reserve(workspace.mruOrder.size());
    for (const QString &entry : workspace.mruOrder)
    {
        const QString clamped = ClampString(entry, MAX_UUID_LENGTH);
        if (!clamped.isEmpty() && std::cmp_less(clampedMru.size(), MAX_WINDOWS))
        {
            clampedMru.append(clamped);
        }
    }
    workspace.mruOrder = std::move(clampedMru);
}

Workspace WorkspacePersistence::Read()
{
    const QString path = WorkspaceFilePath();
    QFile file(path);
    if (!file.exists())
    {
        return Workspace{};
    }
    if (!file.open(QIODevice::ReadOnly))
    {
        return Workspace{};
    }
    const QByteArray raw = file.readAll();
    file.close();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError)
    {
        return Workspace{};
    }
    return WorkspaceFromDoc(doc);
}

Workspace WorkspacePersistence::Take()
{
    Workspace ws = Read();
    if (!ws.windows.empty() || !ws.mruOrder.isEmpty())
    {
        // Atomic wipe: write an empty workspace after the read
        // so a mid-restore crash cannot loop. Empty workspaces
        // parse back as empty (no bound violations) so this is
        // idempotent.
        Workspace empty;
        empty.schemaVersion = SCHEMA_VERSION;
        // Post-tabs review-round Low fix: previously the wipe
        // failure was silently `(void)`'d. On a read-only / full
        // profile that meant every subsequent launch replayed
        // the same workspace forever. Log at warning level so
        // the user has some hope of noticing; the read result
        // is still returned (the primary already committed to
        // restoring these windows this launch).
        if (!Write(std::move(empty)))
        {
            qWarning() << "WorkspacePersistence: post-Take wipe failed; the workspace file "
                          "will be re-consumed on the next launch. Check disk space and "
                          "AppDataLocation write permissions.";
        }
    }
    return ws;
}

bool WorkspacePersistence::Write(Workspace workspace)
{
    if (!SessionHistoryManager::IsPublishingEnabled())
    {
        // A `--new-instance` peer must not clobber the primary's
        // persisted workspace. Symmetric with
        // `SessionHistoryManager::SetOpenWindowsAtQuit`.
        return false;
    }
    Sanitize(workspace);

    const QDir dir = DefaultWorkspaceDir();
    if (!dir.exists() && !dir.mkpath(QStringLiteral(".")))
    {
        return false;
    }
    const QString path = dir.filePath(QLatin1String(WORKSPACE_FILE_NAME));
    QSaveFile saveFile(path);
    if (!saveFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
    {
        return false;
    }
    const QJsonDocument doc = BuildDoc(workspace);
    const QByteArray raw = doc.toJson(QJsonDocument::Indented);
    if (saveFile.write(raw) != raw.size())
    {
        saveFile.cancelWriting();
        return false;
    }
    return saveFile.commit();
}

bool WorkspacePersistence::HasPersistedWorkspace()
{
    // Post-tabs review-round Low fix: match the wipe condition
    // in `Take()` (which fires when either `windows` OR
    // `mruOrder` is non-empty). Previously this returned false
    // for an mru-only file, but `Take()` still wiped it -- an
    // asymmetry that could leave the startup restore-vs-splash
    // gate reading "no workspace" while `Take()` felt obliged
    // to wipe one, and a bare mru-only file (no windows) would
    // stay disk-present but be reported as "empty" indefinitely.
    const Workspace ws = Read();
    return !ws.windows.empty() || !ws.mruOrder.isEmpty();
}

void WorkspacePersistence::Clear()
{
    QFile::remove(WorkspaceFilePath());
}

} // namespace slv::persistence
