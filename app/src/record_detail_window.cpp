#include "record_detail_window.hpp"

#include "record_detail_widget.hpp"

#include <QVBoxLayout>

namespace
{
constexpr int WINDOW_INITIAL_WIDTH = 520;
constexpr int WINDOW_INITIAL_HEIGHT = 600;
} // namespace

RecordDetailWindow::RecordDetailWindow(const RecordDetailContent &content, QWidget *parent)
    : QWidget(parent, Qt::Window)
{
    setObjectName(QStringLiteral("RecordDetailWindow"));
    // Self-delete on close so the owner's tracker self-cleans via
    // `destroyed`. Parent-destruction also fires `destroyed`, so the
    // tracker is correct on both teardown paths.
    setAttribute(Qt::WA_DeleteOnClose);

    const QString titleSummary = content.valid && !content.summary.isEmpty() ? content.summary : tr("Record Details");
    setWindowTitle(tr("Record Details \u2014 %1").arg(titleSummary));
    resize(WINDOW_INITIAL_WIDTH, WINDOW_INITIAL_HEIGHT);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    mWidget = new RecordDetailWidget(this);
    mWidget->SetContent(content);
    // Already in a new window -- another tear-off button would just
    // multiply identical snapshots.
    mWidget->SetOpenInNewWindowVisible(false);
    layout->addWidget(mWidget);
}
