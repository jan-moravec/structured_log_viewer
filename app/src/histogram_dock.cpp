#include "histogram_dock.hpp"

#include "histogram_model.hpp"
#include "histogram_widget.hpp"

#include <QCloseEvent>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

HistogramDock::HistogramDock(LogModel *model, ThemeControl *theme, AnchorManager *anchors, QWidget *parent)
    : QDockWidget(tr("Histogram"), parent)
{
    setObjectName(QStringLiteral("histogramDock"));
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    mModel = new HistogramModel(model, anchors, this);
    mWidget = new HistogramWidget(mModel, theme, this);
    setWidget(mWidget);

    connect(mWidget, &HistogramWidget::bucketClicked, this, &HistogramDock::bucketClicked);
    connect(mWidget, &HistogramWidget::anchorClicked, this, &HistogramDock::anchorClicked);
    connect(mWidget, &HistogramWidget::timeRangeSelected, this, &HistogramDock::timeRangeSelected);
}

void HistogramDock::closeEvent(QCloseEvent *event)
{
    // Mirror `AnchorsDock::closeEvent`: let the base class run first
    // so `closed()` fires only when the close actually goes through
    // (not on a vetoed close, which would leave the toggle out of
    // sync with a still-visible dock).
    QDockWidget::closeEvent(event);
    if (event->isAccepted())
    {
        emit closed();
    }
}
