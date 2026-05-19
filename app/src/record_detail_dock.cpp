#include "record_detail_dock.hpp"

#include "log_model.hpp"
#include "record_detail_widget.hpp"

#include <QObject>

namespace
{
constexpr int DOCK_INITIAL_WIDTH = 400;
}

RecordDetailDock::RecordDetailDock(LogModel *model, QWidget *parent)
    : QDockWidget(QObject::tr("Record Details"), parent), mModel(model)
{
    setObjectName(QStringLiteral("recordDetailDock"));
    // Allow docking on either side, float as a top-level window, and
    // close via the title bar. The View menu's `toggleViewAction()`
    // mirrors the open/closed state.
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    mWidget = new RecordDetailWidget(this);
    setWidget(mWidget);
    setMinimumWidth(DOCK_INITIAL_WIDTH);

    connect(mWidget, &RecordDetailWidget::openInNewWindowRequested, this, &RecordDetailDock::OnOpenInNewWindowRequested);

    Clear();
}

void RecordDetailDock::ShowSourceRow(int sourceRow)
{
    if (!mModel)
    {
        Clear();
        return;
    }
    if (sourceRow < 0 || sourceRow >= mModel->rowCount())
    {
        Clear();
        return;
    }
    mCurrentSourceRow = sourceRow;
    RefreshFromModel();
}

void RecordDetailDock::Clear()
{
    mCurrentSourceRow = -1;
    RecordDetailContent placeholder;
    placeholder.valid = false;
    placeholder.placeholderText =
        QObject::tr("Select a row in the table to inspect it here, or double-click any row to open this pane.");
    mWidget->SetContent(placeholder);
}

void RecordDetailDock::RefreshFromModel()
{
    if (!mModel || mCurrentSourceRow < 0)
    {
        Clear();
        return;
    }
    mWidget->SetContent(BuildRecordDetailContent(*mModel, mCurrentSourceRow));
}

void RecordDetailDock::OnOpenInNewWindowRequested()
{
    // Owner handles the actual window creation; we relay with the
    // current row so the owner can build a frozen snapshot tied to
    // that record.
    emit openInNewWindowRequested(mCurrentSourceRow);
}
