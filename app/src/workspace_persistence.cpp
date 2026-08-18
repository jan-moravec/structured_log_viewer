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
#include <QSet>
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

// Preserve empty strings while bounding persisted identifiers.
QString ClampString(const QString &value, std::size_t maxLen)
{
    if (std::cmp_less_equal(value.size(), maxLen))
    {
        return value;
    }
    return value.left(static_cast<int>(maxLen));
}

// Saved Qt state is indivisible; drop oversized blobs instead of truncating them.
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

// A null UUID marks a rejected window; valid decoded UUIDs are non-null.
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
    // Assign an empty non-null value before decoding so it cannot match the rejection sentinel.
    window.windowUuid = QString(QLatin1String(""));
    window.windowUuid =
        ClampString(obj.value(QLatin1String(KEY_WINDOW_UUID)).toString(), WorkspacePersistence::MAX_UUID_LENGTH);
    // Bound encoded text before decoding so malformed input cannot allocate an oversized buffer.
    const auto maxGeoB64Chars = static_cast<qsizetype>((WorkspacePersistence::MAX_GEOMETRY_BYTES * 4 + 2) / 3 + 4);
    const auto maxDockB64Chars = static_cast<qsizetype>((WorkspacePersistence::MAX_DOCK_STATE_BYTES * 4 + 2) / 3 + 4);
    const QString geoStr = obj.value(QLatin1String(KEY_GEOMETRY)).toString();
    const QString dockStr = obj.value(QLatin1String(KEY_DOCK_STATE)).toString();
    if (geoStr.size() > maxGeoB64Chars || dockStr.size() > maxDockB64Chars)
    {
        // Reject the window rather than decoding or truncating invalid saved state.
        return InvalidWindow();
    }
    const QByteArray geoB64 = geoStr.toLatin1();
    window.geometry = ClampBytes(QByteArray::fromBase64(geoB64), WorkspacePersistence::MAX_GEOMETRY_BYTES);
    const QByteArray dockB64 = dockStr.toLatin1();
    window.dockState = ClampBytes(QByteArray::fromBase64(dockB64), WorkspacePersistence::MAX_DOCK_STATE_BYTES);
    window.activeTabIndex = obj.value(QLatin1String(KEY_ACTIVE_TAB_INDEX)).toInt(0);
    const QJsonArray tabsArr = obj.value(QLatin1String(KEY_TABS)).toArray();
    // Read-side count violations reject the workspace instead of silently losing tabs.
    if (std::cmp_greater(tabsArr.size(), WorkspacePersistence::MAX_TABS_PER_WINDOW))
    {
        return InvalidWindow();
    }
    // Reserve no more than the accepted bound.
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
        // Empty strips and invalid indices both use the first-slot default.
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
        // Preserve incompatible files on disk while treating them as absent.
        return workspace;
    }
    workspace.schemaVersion = version;
    const QJsonArray windowsArr = root.value(QLatin1String(KEY_WINDOWS)).toArray();
    // Read-side count violations reject the workspace instead of silently losing windows.
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
            // One rejected window invalidates the complete snapshot.
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
    // Workspace and session history share an application-data directory, not a transaction.
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
            // Empty strips and invalid indices both use the first-slot default.
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
        // Replace the consumed snapshot with a valid empty document.
        Workspace empty;
        empty.schemaVersion = SCHEMA_VERSION;
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
        // Non-publishing instances must not replace the primary instance's workspace.
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
    const Workspace ws = Read();
    return !ws.windows.empty() || !ws.mruOrder.isEmpty();
}

void WorkspacePersistence::Clear()
{
    QFile::remove(WorkspaceFilePath());
}

namespace
{

Workspace &DeferredWindowsSlot()
{
    static Workspace deferred;
    return deferred;
}

} // namespace

WorkspacePersistence::RestorePlan WorkspacePersistence::PlanRestore(Workspace workspace, std::size_t restoreCap)
{
    RestorePlan plan;
    plan.deferred.schemaVersion = workspace.schemaVersion;
    plan.deferred.mruOrder = workspace.mruOrder;
    if (restoreCap == 0 || workspace.windows.size() <= restoreCap)
    {
        plan.toRestore = std::move(workspace);
        return plan;
    }
    const auto split = static_cast<std::vector<WorkspaceWindow>::difference_type>(restoreCap);
    plan.deferred.windows.assign(workspace.windows.begin() + split, workspace.windows.end());
    workspace.windows.resize(restoreCap);
    plan.toRestore = std::move(workspace);
    return plan;
}

Workspace WorkspacePersistence::MergeCapturedWithDeferred(Workspace captured, const Workspace &deferred)
{
    QSet<QString> seenWindows;
    seenWindows.reserve(static_cast<qsizetype>(captured.windows.size() + deferred.windows.size()));
    for (const WorkspaceWindow &window : captured.windows)
    {
        if (!window.windowUuid.isEmpty())
        {
            seenWindows.insert(window.windowUuid);
        }
    }
    for (const WorkspaceWindow &window : deferred.windows)
    {
        if (window.windowUuid.isEmpty() || seenWindows.contains(window.windowUuid))
        {
            continue;
        }
        seenWindows.insert(window.windowUuid);
        captured.windows.push_back(window);
    }

    QSet<QString> seenMru;
    QStringList mergedMru;
    const auto appendMru = [&](const QStringList &order) {
        for (const QString &uuid : order)
        {
            if (uuid.isEmpty() || seenMru.contains(uuid))
            {
                continue;
            }
            seenMru.insert(uuid);
            mergedMru.append(uuid);
        }
    };
    appendMru(captured.mruOrder);
    appendMru(deferred.mruOrder);
    captured.mruOrder = std::move(mergedMru);
    if (captured.schemaVersion == 0 && deferred.schemaVersion != 0)
    {
        captured.schemaVersion = deferred.schemaVersion;
    }
    return captured;
}

void WorkspacePersistence::SetDeferredWindows(Workspace deferred)
{
    DeferredWindowsSlot() = std::move(deferred);
}

Workspace WorkspacePersistence::TakeDeferredWindows()
{
    Workspace out;
    std::swap(out, DeferredWindowsSlot());
    return out;
}

WorkspacePersistence::RestorePlan WorkspacePersistence::LoadForLaunch(std::size_t restoreCap)
{
    RestorePlan plan = PlanRestore(Read(), restoreCap);
    if (plan.deferred.windows.empty())
    {
        (void)Take();
        SetDeferredWindows({});
    }
    else
    {
        SetDeferredWindows(plan.deferred);
    }
    return plan;
}

} // namespace slv::persistence
