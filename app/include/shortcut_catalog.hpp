#pragma once

#include <QKeySequence>
#include <QList>
#include <QString>

QT_BEGIN_NAMESPACE
class QAction;
class QMainWindow;
QT_END_NAMESPACE

namespace ShortcutCatalog
{
/// One row in a `ShortcutGroup`: pre-cleaned text plus the native
/// shortcut string. Captured eagerly so the empty-state painter
/// does not have to mutate `QAction` text on every frame.
struct Entry
{
    QString text;
    QString shortcut;
};

/// Group of shortcut rows under a heading (typically the owning
/// `QMenu` title; falls back to "Other" for actions added directly
/// to the window with `addAction()`).
struct Group
{
    QString title;
    QList<Entry> entries;
};

/// Walk @p root's menu bar in declaration order, then the orphan
/// actions, and produce one `Group` per heading. Skips actions
/// without a shortcut, separators, and actions whose `text()` is
/// empty. `&` accelerator markers are stripped from both the
/// menu title and each action label so the rendered card reads
/// cleanly.
[[nodiscard]] QList<Group> Build(const QMainWindow *root);
} // namespace ShortcutCatalog
