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
    // Capture the pre-reveal focus only on a real reveal: a second
    // call while the bar is already open should not overwrite the
    // saved target with the find edit itself.
    if (!isVisible())
    {
        QWidget *current = QApplication::focusWidget();
        // Don't re-stash a pointer into our own subtree -- if the
        // bar was hidden but somehow holds focus (test path), the
        // restore would just bounce back into the find edit.
        if (current != nullptr && !isAncestorOf(current))
        {
            mFocusBeforeReveal = current;
        }
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
