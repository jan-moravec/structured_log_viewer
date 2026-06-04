#include "find_dock.hpp"

#include "find_record_widget.hpp"

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
    if (!isVisible())
    {
        show();
    }
    raise();
    mWidget->SetEditFocus();
}
