#include "shortcut_catalog.hpp"

#include <QAction>
#include <QCoreApplication>
#include <QMainWindow>
#include <QMenu>
#include <QMenuBar>
#include <QSet>

namespace ShortcutCatalog
{
namespace
{
/// Strip single `&` accelerator markers while preserving literal
/// `&&` (which Qt renders as a literal `&` in menus). No-op on
/// strings without accelerators.
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

/// Push @p action onto @p group, recording the cleaned label and
/// native-text shortcut. Idempotent on a per-Group basis.
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
        // Menu-bar order is the user-facing order (File, Edit, View,
        // ...). Drives the visual ordering of the empty-state card and
        // the shortcuts dialog.
        for (const QAction *menuAction : bar->actions())
        {
            QMenu *menu = menuAction->menu();
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

    // Orphan actions registered via `QWidget::addAction()` directly on
    // the main window — e.g. the anchor hotkeys (Ctrl+0..8, F2,
    // Shift+F2) and the Record Details toggle. `findChildren<QAction*>`
    // would also drag in actions owned by child dialogs (Preferences,
    // Filter editor, ...) which are not user-invocable from the main
    // window, so we walk the window's own action list instead.
    Group other;
    other.title = QCoreApplication::translate("ShortcutCatalog", "Other");
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
} // namespace ShortcutCatalog
