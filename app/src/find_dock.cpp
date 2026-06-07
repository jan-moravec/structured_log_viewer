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
    // Find bars belong on the top or bottom edge; a vertical side dock
    // would squeeze the search field into a useless narrow column.
    setAllowedAreas(Qt::TopDockWidgetArea | Qt::BottomDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    mWidget = new FindRecordWidget(this);
    setWidget(mWidget);
}

void FindDock::RevealAndFocus()
{
    // Refresh the stash on every reveal whose source focus is outside
    // our subtree, so Ctrl+F invoked while the bar is already on-screen
    // (but focus has drifted back to the table) picks up the current
    // target instead of a stale one from the very first reveal.
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
    // Snapshot + clear before forwarding so a re-entrant close (e.g.
    // a focusOut handler that triggers another close) can't double-fire
    // the restore.
    QWidget *target = mFocusBeforeReveal.data();
    mFocusBeforeReveal.clear();
    QDockWidget::closeEvent(event);
    if (!event->isAccepted())
    {
        return;
    }
    if (target != nullptr && target->isVisible())
    {
        target->setFocus(Qt::OtherFocusReason);
    }
    emit closed();
}

void FindDock::showEvent(QShowEvent *event)
{
    QDockWidget::showEvent(event);
    emit revealed();
}
