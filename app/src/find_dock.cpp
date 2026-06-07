#include "find_dock.hpp"

#include "find_record_widget.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QShowEvent>
#include <QWidget>

FindDock::FindDock(QWidget *parent)
    : QDockWidget(tr("Find"), parent)
{
    setObjectName(QStringLiteral("findDock"));
    // Find bars belong at the top or the bottom of the editor; a
    // vertical side dock would squeeze the search field into a
    // useless narrow column.
    setAllowedAreas(Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    mWidget = new FindRecordWidget(this);
    setWidget(mWidget);
}

void FindDock::RevealAndFocus()
{
    // Refresh `mFocusBeforeReveal` on *every* reveal whose source
    // focus is outside our subtree. Earlier we gated this on
    // `!isVisible()`, which meant a Ctrl+F invoked while the bar
    // was already on-screen (but the user had clicked back into
    // the table view) kept the stash pointing at whatever was
    // focused on the very first reveal -- often a widget that no
    // longer existed or no longer made sense to restore. The
    // `isAncestorOf` guard still prevents a focus already inside
    // the bar (the edit, the regex toggle, ...) from overwriting
    // the saved target with itself; otherwise dismissing the bar
    // would just bounce focus back into the bar's own widget tree.
    QWidget *current = QApplication::focusWidget();
    if (current != nullptr && !isAncestorOf(current))
    {
        mFocusBeforeReveal = current;
    }
    if (!isVisible())
    {
        show();
    }
    raise();
    mWidget->SetEditFocus();
}

void FindDock::closeEvent(QCloseEvent *event)
{
    // Snapshot + clear before forwarding so a re-entrant close
    // (e.g. inside a focusOut handler that triggers another
    // close) can't double-fire the restore.
    QWidget *target = mFocusBeforeReveal.data();
    mFocusBeforeReveal.clear();
    QDockWidget::closeEvent(event);
    if (!event->isAccepted())
    {
        return;
    }
    // Skip the restore for the construction-time hide (no saved
    // target) and for races where the previously-focused widget
    // was torn down while the bar was open.
    if (target != nullptr && target->isVisible())
    {
        target->setFocus(Qt::OtherFocusReason);
    }
    emit closed();
}

void FindDock::showEvent(QShowEvent *event)
{
    QDockWidget::showEvent(event);
    // `revealed` fires for cold reveals *and* for tab activations
    // inside a tabified group; both are moments when the user is
    // about to look at the bar and any stale match count needs to
    // catch up to model activity that happened while it was
    // invisible.
    emit revealed();
}
