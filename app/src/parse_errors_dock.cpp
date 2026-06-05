#include "parse_errors_dock.hpp"

#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QPushButton>
#include <QShortcut>
#include <QStringList>
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

/// `Qt::UserRole` flag set on the trailing "+N more dropped"
/// footer item so `TrimToCap` can replace it in place instead of
/// minting a new one each batch.
constexpr int OVERFLOW_FOOTER_ROLE = Qt::UserRole + 1;
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
    // lines via Ctrl+C.
    mList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mList->setUniformItemSizes(false);
    mList->setTextElideMode(Qt::ElideMiddle);
    mList->setAlternatingRowColors(true);
    layout->addWidget(mList, /*stretch=*/1);

    setWidget(body);
    setMinimumWidth(DOCK_MIN_WIDTH);

    connect(mClearButton, &QPushButton::clicked, this, &ParseErrorsDock::ClearErrors);

    // Ctrl+C copies the selected error rows. Scoped to the list
    // so it doesn't shadow window-level Copy when focus is
    // elsewhere. Without this, `setSelectionMode(ExtendedSelection)`
    // would let the user select rows that they couldn't actually
    // copy.
    auto *copyShortcut = new QShortcut(QKeySequence::Copy, mList);
    copyShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copyShortcut, &QShortcut::activated, this, &ParseErrorsDock::CopySelection);

    RefreshSummary();
}

void ParseErrorsDock::AppendErrors(const QString &title, const std::vector<std::string> &errors)
{
    if (errors.empty())
    {
        return;
    }

    const QIcon warningIcon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning);

    // Auto-raise only on the first batch of a session (i.e. when
    // the dock was previously empty). Subsequent batches update
    // silently and rely on the status-bar indicator -- a streaming
    // user who has explicitly closed the dock should not have it
    // pop back open every batch.
    const bool firstBatchOfSession = mErrorCount == 0 && mDroppedCount == 0;

    // The trailing overflow footer (if any) must move to the
    // bottom after the new entries land. Drop it before the
    // append so we can re-mint it once cap-trimming settles.
    for (int i = mList->count() - 1; i >= 0; --i)
    {
        QListWidgetItem *item = mList->item(i);
        if (item != nullptr && item->data(OVERFLOW_FOOTER_ROLE).toBool())
        {
            delete mList->takeItem(i);
            break;
        }
    }

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
        ++mErrorCount;
    }

    TrimToCap();

    mList->scrollToBottom();
    if (firstBatchOfSession)
    {
        if (!isVisible())
        {
            show();
        }
        raise();
    }

    RefreshSummary();
}

void ParseErrorsDock::ClearErrors()
{
    if (mList->count() == 0 && mErrorCount == 0 && mDroppedCount == 0)
    {
        return;
    }
    mList->clear();
    mErrorCount = 0;
    mDroppedCount = 0;
    RefreshSummary();
}

void ParseErrorsDock::TrimToCap()
{
    if (mErrorCount <= MAX_DISPLAYED_ERRORS)
    {
        return;
    }
    // Walk from the top, evicting entries until we are back under
    // the cap. Group headers (`ItemIsEnabled` only) don't count
    // toward `mErrorCount`; they ride along when evicting from
    // their batch, but we never strand a header above its
    // surviving rows -- after the loop we sweep front-side
    // headers only when they are immediately followed by another
    // header (orphan), preserving any header that still has rows
    // beneath it.
    while (mList->count() > 0 && mErrorCount > MAX_DISPLAYED_ERRORS)
    {
        QListWidgetItem *item = mList->takeItem(0);
        if (item == nullptr)
        {
            continue;
        }
        if (item->flags().testFlag(Qt::ItemIsSelectable))
        {
            --mErrorCount;
            ++mDroppedCount;
        }
        delete item;
    }

    // Compact orphan headers at the front: a header followed
    // immediately by another header (or by nothing) lost its
    // entire batch and should go. A header followed by an error
    // row stays put -- it still labels surviving entries.
    while (mList->count() > 0)
    {
        QListWidgetItem *first = mList->item(0);
        if (first == nullptr || first->flags().testFlag(Qt::ItemIsSelectable))
        {
            break;
        }
        // First item is a header. Look at its successor.
        QListWidgetItem *second = mList->count() > 1 ? mList->item(1) : nullptr;
        const bool isOrphan = second == nullptr || !second->flags().testFlag(Qt::ItemIsSelectable);
        if (!isOrphan)
        {
            break;
        }
        delete mList->takeItem(0);
    }

    if (mDroppedCount > 0)
    {
        // Single overflow footer at the tail; re-minted on each
        // call (we removed the prior one in `AppendErrors` before
        // adding the new batch) so the count stays current.
        auto *footer = new QListWidgetItem(tr("(\u2026 %n earlier error(s) dropped)", nullptr, mDroppedCount));
        QFont footerFont = footer->font();
        footerFont.setItalic(true);
        footer->setFont(footerFont);
        footer->setFlags(Qt::ItemIsEnabled);
        footer->setData(OVERFLOW_FOOTER_ROLE, true);
        mList->addItem(footer);
    }
}

void ParseErrorsDock::CopySelection() const
{
    if (mList == nullptr)
    {
        return;
    }
    const QList<QListWidgetItem *> selected = mList->selectedItems();
    if (selected.isEmpty())
    {
        return;
    }
    // Preserve list order; `selectedItems` returns selection
    // order which can confuse readers expecting top-to-bottom.
    QStringList lines;
    lines.reserve(selected.size());
    for (int i = 0; i < mList->count(); ++i)
    {
        QListWidgetItem *item = mList->item(i);
        if (item != nullptr && item->isSelected())
        {
            lines.append(item->text());
        }
    }
    QGuiApplication::clipboard()->setText(lines.join(QChar::fromLatin1('\n')));
}

void ParseErrorsDock::RefreshSummary()
{
    if (mErrorCount == 0)
    {
        mSummary->setText(tr("No parse errors."));
    }
    else if (mDroppedCount > 0)
    {
        mSummary->setText(tr("%1 error(s); %2 earlier dropped.").arg(mErrorCount).arg(mDroppedCount));
    }
    else
    {
        mSummary->setText(tr("%n error(s).", nullptr, mErrorCount));
    }
    mClearButton->setEnabled(mErrorCount > 0 || mDroppedCount > 0);
    emit countChanged(mErrorCount);
}
