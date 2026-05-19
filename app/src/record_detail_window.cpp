#include "record_detail_window.hpp"

#include "record_detail_widget.hpp"

#include <QObject>
#include <QVBoxLayout>

namespace
{
constexpr int WINDOW_INITIAL_WIDTH = 520;
constexpr int WINDOW_INITIAL_HEIGHT = 600;
}

RecordDetailWindow::RecordDetailWindow(const RecordDetailContent &content, QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setObjectName(QStringLiteral("RecordDetailWindow"));
    // Free the heap when the user closes the window so the owner's
    // `QPointer` list self-cleans without an explicit handler.
    setAttribute(Qt::WA_DeleteOnClose);

    const QString titleSummary =
        content.valid && !content.summary.isEmpty() ? content.summary : QObject::tr("Record Details");
    setWindowTitle(QObject::tr("Record Details \u2014 %1").arg(titleSummary));
    resize(WINDOW_INITIAL_WIDTH, WINDOW_INITIAL_HEIGHT);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    mWidget = new RecordDetailWidget(this);
    mWidget->SetContent(content);
    // The pop-out is itself a "new window"; offering another tear-off
    // from inside would multiply identical snapshots.
    mWidget->SetOpenInNewWindowVisible(false);
    layout->addWidget(mWidget);
}
