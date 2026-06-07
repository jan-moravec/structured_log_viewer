#include "parse_errors_dock.hpp"

#include <QApplication>
#include <QBrush>
#include <QClipboard>
#include <QCloseEvent>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QLocale>
#include <QPushButton>
#include <QScrollBar>
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

/// Pixel slack on the vertical scrollbar's `max - value` reading
/// below which we treat the user as "still at the tail". Anything
/// inside this band auto-follows new batches; anything outside it
/// preserves the user's scroll position so they can read earlier
/// errors without being yanked back to the end on every append.
/// Sized to absorb a row or two of rounding from
/// `verticalScrollBar()`'s integer reporting.
constexpr int AUTO_FOLLOW_TAIL_SLACK_PX = 4;
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

    // Minimum on the content widget, not the dock: setting it on
    // the dock propagates a 320 px floor onto the entire
    // QMainWindow even while the dock itself is hidden.
    body->setMinimumWidth(DOCK_MIN_WIDTH);
    setWidget(body);

    connect(mClearButton, &QPushButton::clicked, this, &ParseErrorsDock::ClearErrors);

    // Ctrl+C copies the selected error rows. Parented on `body`
    // (not `mList`) with `WidgetWithChildrenShortcut` so the
    // shortcut still fires when the user has clicked the Clear
    // button (which moves focus off the list but leaves the
    // selection intact). Window-level Copy is preserved because
    // the scope still excludes the rest of `MainWindow`.
    auto *copyShortcut = new QShortcut(QKeySequence::Copy, body);
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

    // Decide whether this batch should fire `firstBatchArrived`
    // (which `MainWindow` translates into auto-raising the dock).
    // The flag flips on the first batch *per session* and is
    // cleared only by `ResetSessionState` -- the in-dock Clear
    // button leaves it set, so a user who clicked Clear and
    // dismissed the dock is not yanked back to it the next time a
    // streaming line fails to parse.
    const bool firstBatchOfSession = !mHasSeenFirstBatch;

    // Snapshot the user's scroll position *before* mutating the
    // list. If they were already pinned at the tail (auto-follow
    // case, also the initial empty-list case), we'll re-pin after
    // the append; if they had scrolled up to read earlier errors,
    // we leave the viewport alone so the new batch doesn't yank
    // them out of context. Mirrors the chat / log convention.
    const QScrollBar *vBar = mList->verticalScrollBar();
    const bool wasAtTail =
        vBar == nullptr || (vBar->maximum() - vBar->value()) <= AUTO_FOLLOW_TAIL_SLACK_PX;

    // The trailing overflow footer (if any) must move to the
    // bottom after the new entries land. It is always the last
    // item by construction (`TrimToCap` appends it after every
    // trim), so we only need to inspect the tail rather than walk
    // the list.
    if (const int last = mList->count() - 1; last >= 0)
    {
        QListWidgetItem *tail = mList->item(last);
        if (tail != nullptr && tail->data(OVERFLOW_FOOTER_ROLE).toBool())
        {
            delete mList->takeItem(last);
        }
    }

    // Pre-trim a pathologically large batch before constructing
    // list items. If we instead let `TrimToCap` walk the list
    // from the top, it would evict prior batches *and* the new
    // batch's own group header before reaching the rows that
    // outgrew the cap, leaving the surviving rows visually
    // headerless. Skipping the leading slice up front keeps the
    // header pinned to the rows it labels and avoids minting
    // QListWidgetItem objects we'll only delete a moment later.
    size_t errorsBegin = 0;
    if (errors.size() > static_cast<size_t>(MAX_DISPLAYED_ERRORS))
    {
        errorsBegin = errors.size() - static_cast<size_t>(MAX_DISPLAYED_ERRORS);
        mDroppedCount += static_cast<int>(errorsBegin);
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

    for (size_t i = errorsBegin; i < errors.size(); ++i)
    {
        auto *item = new QListWidgetItem(warningIcon, QString::fromStdString(errors[i]));
        item->setToolTip(QString::fromStdString(errors[i]));
        item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
        mList->addItem(item);
        ++mErrorCount;
    }

    TrimToCap();
    // Footer rebuild is intentionally separate from `TrimToCap`:
    // a single batch sized exactly at the cap pre-trims (bumping
    // `mDroppedCount`) without ever pushing `mErrorCount` past
    // `MAX_DISPLAYED_ERRORS`, so `TrimToCap` has no eviction work
    // and would skip its tail block. The footer must reflect
    // `mDroppedCount` regardless of which path got us there.
    RebuildOverflowFooter();

    // Re-pin to the tail only when the user was already there.
    // Otherwise the new batch lands silently and the user keeps
    // reading whatever earlier rows they had scrolled up to. The
    // status-bar indicator already surfaces the running count so
    // they can tell something arrived without being interrupted.
    if (wasAtTail)
    {
        mList->scrollToBottom();
    }
    RefreshSummary();
    if (firstBatchOfSession)
    {
        // Latch the flag before emitting so a re-entrant
        // `AppendErrors` from a slot connected to
        // `firstBatchArrived` (defensive — none today) cannot
        // double-fire it.
        mHasSeenFirstBatch = true;
        // Let `MainWindow` decide whether to actually raise the
        // dock -- the dock itself does not know about the rest of
        // the GUI chrome and shouldn't yank focus off the find
        // bar (or any future tabified neighbour).
        emit firstBatchArrived();
    }
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
    // `mHasSeenFirstBatch` deliberately *not* reset: the user
    // already chose to dismiss this session's errors; popping the
    // dock back open on the next streaming hiccup would be the
    // bug we are guarding against. `ResetSessionState` is the
    // canonical path to re-arm the auto-raise.
    RefreshSummary();
}

void ParseErrorsDock::ResetSessionState()
{
    // Same surface as ClearErrors plus the auto-raise re-arm.
    // Skip the work entirely if we are already in the pristine
    // post-construction state to avoid a spurious `countChanged`
    // emit on every session boundary in idle apps.
    if (mList->count() == 0 && mErrorCount == 0 && mDroppedCount == 0 && !mHasSeenFirstBatch)
    {
        return;
    }
    mList->clear();
    mErrorCount = 0;
    mDroppedCount = 0;
    mHasSeenFirstBatch = false;
    RefreshSummary();
}

void ParseErrorsDock::closeEvent(QCloseEvent *event)
{
    QDockWidget::closeEvent(event);
    if (event->isAccepted())
    {
        emit closed();
    }
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
    // their batch. Remember the text + font of the most recently
    // evicted header so that, if its batch survives in part, we
    // can re-mint a label for the orphaned rows below -- otherwise
    // they'd float headerless above the next batch's title.
    QString lastEvictedHeaderText;
    QFont lastEvictedHeaderFont;
    bool haveEvictedHeader = false;
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
        else if (!item->data(OVERFLOW_FOOTER_ROLE).toBool())
        {
            // Group header. Stash for possible re-insertion below.
            // (The overflow footer is removed by `AppendErrors`
            // before this method runs, so we shouldn't actually
            // see one here -- but guard anyway.)
            lastEvictedHeaderText = item->text();
            lastEvictedHeaderFont = item->font();
            haveEvictedHeader = true;
        }
        delete item;
    }

    // Re-mint the most recently evicted header if its first
    // surviving row is now stranded at the top of the list (i.e.
    // a selectable row with no preceding header). Without this,
    // partial-batch eviction would leave error rows above the
    // next batch's title with no label of their own -- the user
    // would see a wall of errors that look like they belong to
    // the *next* batch's header sitting below them.
    if (haveEvictedHeader && mList->count() > 0)
    {
        QListWidgetItem *first = mList->item(0);
        if (first != nullptr && first->flags().testFlag(Qt::ItemIsSelectable))
        {
            auto *replacement = new QListWidgetItem(lastEvictedHeaderText);
            replacement->setFont(lastEvictedHeaderFont);
            // Disable + non-selectable to match the header style
            // minted in `AppendErrors`. Arrow-key nav steps over
            // it; Ctrl+A skips it.
            replacement->setFlags(Qt::ItemIsEnabled);
            mList->insertItem(0, replacement);
        }
    }

    // Compact orphan headers at the front: a header followed
    // immediately by another header (or by nothing) lost its
    // entire batch and should go. A header followed by an error
    // row stays put -- it still labels surviving entries. This
    // also catches the (unusual) case where the re-mint above
    // produced a redundant pair, e.g. the evicted batch had only
    // its header survive after the eviction-then-reinsert dance.
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
}

void ParseErrorsDock::RebuildOverflowFooter()
{
    if (mDroppedCount <= 0)
    {
        return;
    }
    // Pin the contract: caller (`AppendErrors`) is responsible
    // for stripping any prior footer before we run, so this
    // method only ever *appends* a fresh footer. A future
    // refactor that calls `RebuildOverflowFooter` from a path
    // that doesn't strip the tail would silently grow a stack
    // of duplicate footers; the assert blows up loudly in debug.
    Q_ASSERT(
        mList->count() == 0 || !mList->item(mList->count() - 1)->data(OVERFLOW_FOOTER_ROLE).toBool()
    );
    // Single overflow footer at the tail. `AppendErrors` strips
    // any prior footer at the top of the call, so this method
    // only ever appends -- never compares-and-replaces. `%Ln`
    // matches the locale-grouped digits used in the summary
    // header (no jitter when counts cross 1k / 1M).
    auto *footer = new QListWidgetItem(tr("(\u2026 %Ln earlier error(s) dropped)", nullptr, mDroppedCount));
    QFont footerFont = footer->font();
    footerFont.setItalic(true);
    footer->setFont(footerFont);
    footer->setFlags(Qt::ItemIsEnabled);
    footer->setData(OVERFLOW_FOOTER_ROLE, true);
    mList->addItem(footer);
}

void ParseErrorsDock::CopySelection() const
{
    if (mList == nullptr)
    {
        return;
    }
    if (mList->selectedItems().isEmpty())
    {
        return;
    }

    // Group headers and the overflow footer have `Qt::ItemIsEnabled`
    // only (no `ItemIsSelectable`), so the user can never select
    // them directly. Walk the list once and synthesise the header
    // above the first selected row in each group plus the overflow
    // footer at the bottom, so the pasted block reads as
    // self-contained text rather than orphaned messages.
    QStringList lines;
    const int count = mList->count();
    QListWidgetItem *currentHeader = nullptr;
    bool currentHeaderEmitted = false;
    bool sawAnyError = false;
    for (int i = 0; i < count; ++i)
    {
        QListWidgetItem *item = mList->item(i);
        if (item == nullptr)
        {
            continue;
        }
        const bool isSelectable = item->flags().testFlag(Qt::ItemIsSelectable);
        if (!isSelectable)
        {
            // Either a group header or the overflow footer; neither
            // is user-selectable. Headers reset the per-group
            // tracking; the footer is handled after the main loop.
            if (!item->data(OVERFLOW_FOOTER_ROLE).toBool())
            {
                currentHeader = item;
                currentHeaderEmitted = false;
            }
            continue;
        }
        if (!item->isSelected())
        {
            continue;
        }
        if (currentHeader != nullptr && !currentHeaderEmitted)
        {
            lines.append(currentHeader->text());
            currentHeaderEmitted = true;
        }
        lines.append(item->text());
        sawAnyError = true;
    }
    if (sawAnyError)
    {
        // Append the overflow footer (if any) so the dropped count
        // travels with the copied excerpt. Mirrors the dock's own
        // header summary.
        for (int i = count - 1; i >= 0; --i)
        {
            QListWidgetItem *item = mList->item(i);
            if (item != nullptr && item->data(OVERFLOW_FOOTER_ROLE).toBool())
            {
                lines.append(item->text());
                break;
            }
        }
    }
    if (lines.isEmpty())
    {
        return;
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
        // Two independent counts -> can't use a single `%Ln`
        // plural form; format both via QLocale so digit grouping
        // matches the rest of the GUI ("12,345 errors; 1,000
        // earlier dropped").
        const QLocale locale = QLocale::system();
        mSummary->setText(tr("%1 error(s); %2 earlier dropped.")
                              .arg(locale.toString(static_cast<qlonglong>(mErrorCount)))
                              .arg(locale.toString(static_cast<qlonglong>(mDroppedCount))));
    }
    else
    {
        mSummary->setText(tr("%Ln error(s).", nullptr, mErrorCount));
    }
    mClearButton->setEnabled(mErrorCount > 0 || mDroppedCount > 0);
    emit countChanged(mErrorCount, mDroppedCount);
}
