#pragma once

#include <QKeySequence>
#include <QList>
#include <QString>

QT_BEGIN_NAMESPACE
class QAction;
class QMainWindow;
QT_END_NAMESPACE

namespace shortcut_catalog
{
/// One row in a `Group`: cleaned action text and its native shortcut.
struct Entry
{
    QString text;
    QString shortcut;
};

/// A heading (usually a menu title, or "Other") and its shortcut rows.
struct Group
{
    QString title;
    QList<Entry> entries;
};

/// Builds one `Group` per menu in declaration order, plus a trailing
/// "Other" group for window-level orphan actions. Skips separators
/// and actions without a shortcut, and strips `&` accelerator markers.
[[nodiscard]] QList<Group> Build(const QMainWindow *root);
} // namespace shortcut_catalog
