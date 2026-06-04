#include "shortcut_catalog.hpp"

#include <QAction>
#include <QCoreApplication>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QSet>

namespace shortcut_catalog
{
namespace
{
/// Strips `&` accelerator markers while preserving `&&` (literal `&`).
QString CleanAccelerators(const QString &raw)
{
    if (!raw.contains(QLatin1Char('&')))
    {
        return raw;
    }
    constexpr QChar SENTINEL{0x1A};
    QString out = raw;
    out.replace(QStringLiteral("&&"), QString(SENTINEL));
    out.remove(QLatin1Char('&'));
    out.replace(SENTINEL, QLatin1Char('&'));
    return out;
}

/// True for actions that should appear in the catalog.
bool IsListable(const QAction *action)
{
    if (action == nullptr || action->isSeparator())
    {
        return false;
    }
    if (action->shortcut().isEmpty())
    {
        return false;
    }
    return !action->text().isEmpty();
}

/// Appends an entry for @p action with its cleaned label and shortcut.
void AppendEntry(Group &group, const QAction *action)
{
    Entry entry;
    entry.text = CleanAccelerators(action->text());
    entry.shortcut = action->shortcut().toString(QKeySequence::NativeText);
    group.entries.append(std::move(entry));
}
} // namespace

QList<Group> Build(const QMainWindow *root)
{
    QList<Group> result;
    if (root == nullptr)
    {
        return result;
    }

    QSet<const QAction *> claimed;

    if (const auto *bar = root->menuBar(); bar != nullptr)
    {
        // Menu-bar order is the user-facing order (File, Edit, View, ...).
        for (const QAction *menuAction : bar->actions())
        {
            const QMenu *menu = menuAction->menu();
            if (menu == nullptr)
            {
                continue;
            }
            Group group;
            group.title = CleanAccelerators(menu->title());
            for (const QAction *action : menu->actions())
            {
                if (!IsListable(action))
                {
                    continue;
                }
                AppendEntry(group, action);
                claimed.insert(action);
            }
            if (!group.entries.isEmpty())
            {
                result.append(std::move(group));
            }
        }
    }

    // Orphan actions added directly via `addAction()` (anchor hotkeys,
    // Record Details toggle, ...). We walk `root->actions()` instead of
    // `findChildren` to skip actions owned by child dialogs.
    Group other;
    other.title = QCoreApplication::translate("shortcut_catalog", "Other");
    for (const QAction *action : root->actions())
    {
        if (!IsListable(action) || claimed.contains(action))
        {
            continue;
        }
        AppendEntry(other, action);
    }
    if (!other.entries.isEmpty())
    {
        result.append(std::move(other));
    }

    return result;
}
} // namespace shortcut_catalog
