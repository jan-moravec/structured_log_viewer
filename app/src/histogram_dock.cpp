#include "histogram_dock.hpp"

#include "histogram_model.hpp"
#include "histogram_widget.hpp"

#include <QCloseEvent>
#include <QString>
#include <QVBoxLayout>
#include <QWidget>

HistogramDock::HistogramDock(LogModel *model, ThemeControl *theme, QWidget *parent) : QDockWidget(tr("Histogram"), parent)
{
    setObjectName(QStringLiteral("histogramDock"));
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    mModel = new HistogramModel(model, this);
    mWidget = new HistogramWidget(mModel, theme, this);
    setWidget(mWidget);

    connect(mWidget, &HistogramWidget::bucketClicked, this, &HistogramDock::bucketClicked);
    connect(mWidget, &HistogramWidget::timeRangeSelected, this, &HistogramDock::timeRangeSelected);
}

void HistogramDock::closeEvent(QCloseEvent *event)
{
    emit closed();
    QDockWidget::closeEvent(event);
}
