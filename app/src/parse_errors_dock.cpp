#include "parse_errors_dock.hpp"

#include <QApplication>
#include <QBrush>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
/// Minimum width that still renders a typical "Error Parsing
/// Logs: <one-line message>" entry without the horizontal
/// scrollbar getting in the way. The dock is meant for the bottom
/// edge by default; keep it narrow enough for that.
constexpr int DOCK_MIN_WIDTH = 320;

/// Outer margins / spacing matching the find bar densities.
constexpr int OUTER_MARGIN = 6;
constexpr int HEADER_SPACING = 6;
constexpr int LAYOUT_SPACING = 4;
} // namespace

ParseErrorsDock::ParseErrorsDock(QWidget *parent)
    : QDockWidget(tr("Parse Errors"), parent)
{
    setObjectName(QStringLiteral("parseErrorsDock"));
    setAllowedAreas(Qt::AllDockWidgetAreas);
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    auto *body = new QWidget(this);
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(OUTER_MARGIN, OUTER_MARGIN, OUTER_MARGIN, OUTER_MARGIN);
    layout->setSpacing(LAYOUT_SPACING);

    auto *header = new QHBoxLayout();
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(HEADER_SPACING);

    mSummary = new QLabel(body);
    mSummary->setObjectName(QStringLiteral("parseErrorsSummary"));
    mSummary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    header->addWidget(mSummary, /*stretch=*/1);

    mClearButton = new QPushButton(tr("Clear"), body);
    mClearButton->setObjectName(QStringLiteral("parseErrorsClear"));
    mClearButton->setFlat(true);
    mClearButton->setCursor(Qt::PointingHandCursor);
    mClearButton->setEnabled(false);
    header->addWidget(mClearButton);

    layout->addLayout(header);

    mList = new QListWidget(body);
    mList->setObjectName(QStringLiteral("parseErrorsList"));
    // Read-only: parse errors are diagnostic data, not something
    // the user edits. Selectable so they can copy individual
    // lines.
    mList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mList->setUniformItemSizes(false);
    mList->setTextElideMode(Qt::ElideMiddle);
    mList->setAlternatingRowColors(true);
    layout->addWidget(mList, /*stretch=*/1);

    setWidget(body);
    setMinimumWidth(DOCK_MIN_WIDTH);

    connect(mClearButton, &QPushButton::clicked, this, &ParseErrorsDock::ClearErrors);
    RefreshSummary();
}

void ParseErrorsDock::AppendErrors(const QString &title, const std::vector<std::string> &errors)
{
    if (errors.empty())
    {
        return;
    }

    const QIcon warningIcon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning);

    // Group header so consecutive batches stay visually grouped
    // even after the user scrolls into them. The header is
    // disabled (non-selectable in keyboard nav) so arrow keys
    // step over it.
    auto *headerItem = new QListWidgetItem(title);
    QFont headerFont = headerItem->font();
    headerFont.setBold(true);
    headerItem->setFont(headerFont);
    headerItem->setFlags(Qt::ItemIsEnabled);
    mList->addItem(headerItem);

    for (const std::string &error : errors)
    {
        auto *item = new QListWidgetItem(warningIcon, QString::fromStdString(error));
        item->setToolTip(QString::fromStdString(error));
        item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        mList->addItem(item);
    }

    // Surface the new entries: scroll to the latest, then
    // show + raise the dock so a hidden / tabified one comes to
    // the front.
    mList->scrollToBottom();
    if (!isVisible())
    {
        show();
    }
    raise();

    RefreshSummary();
}

void ParseErrorsDock::ClearErrors()
{
    if (mList->count() == 0)
    {
        return;
    }
    mList->clear();
    RefreshSummary();
}

int ParseErrorsDock::Count() const noexcept
{
    if (mList == nullptr)
    {
        return 0;
    }
    // The list interleaves group-header items with error rows;
    // only error rows count toward the user-visible total.
    int count = 0;
    for (int i = 0; i < mList->count(); ++i)
    {
        const QListWidgetItem *item = mList->item(i);
        if (item == nullptr)
        {
            continue;
        }
        // Group headers are flagged `ItemIsEnabled` only; error
        // rows are also `ItemIsSelectable`.
        if (item->flags().testFlag(Qt::ItemIsSelectable))
        {
            ++count;
        }
    }
    return count;
}

void ParseErrorsDock::RefreshSummary()
{
    const int count = Count();
    if (count == 0)
    {
        mSummary->setText(tr("No parse errors."));
    }
    else
    {
        mSummary->setText(tr("%n error(s).", nullptr, count));
    }
    mClearButton->setEnabled(count > 0);
    emit countChanged(count);
}
