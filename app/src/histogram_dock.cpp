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
    // Mirror `AnchorsDock::closeEvent`: let the base class (and any
    // installed event filters) run first, and only propagate `closed()`
    // when the close is actually going through. Emitting before the
    // base call would fire `closed()` even for a vetoed close and
    // unwire the toggle action from a still-visible dock.
    QDockWidget::closeEvent(event);
    if (event->isAccepted())
    {
        emit closed();
    }
}
