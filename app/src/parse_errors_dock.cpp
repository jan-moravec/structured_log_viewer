#include "parse_errors_dock.hpp"

#include "log_session.hpp"
#include "session_bind_context.hpp"

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

#include <utility>

namespace
{
/// Minimum dock width that fits a typical "Error Parsing Logs:
/// <message>" entry without a horizontal scrollbar.
constexpr int DOCK_MIN_WIDTH = 320;

constexpr int OUTER_MARGIN = 6;
constexpr int HEADER_SPACING = 6;
constexpr int LAYOUT_SPACING = 4;

/// Flag on the trailing overflow footer item so we can identify it.
constexpr int OVERFLOW_FOOTER_ROLE = Qt::UserRole + 1;

/// Slack on the scrollbar's `max - value`; inside this band we treat
/// the user as "still at the tail" and auto-follow new batches.
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
    // Read-only diagnostic data; selectable so Ctrl+C can copy entries.
    mList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mList->setUniformItemSizes(false);
    mList->setTextElideMode(Qt::ElideMiddle);
    mList->setAlternatingRowColors(true);
    layout->addWidget(mList, /*stretch=*/1);

    // Minimum on the content widget, not the dock: setting it on the
    // dock propagates the floor onto the entire QMainWindow even
    // while the dock is hidden.
    body->setMinimumWidth(DOCK_MIN_WIDTH);
    setWidget(body);

    connect(mClearButton, &QPushButton::clicked, this, &ParseErrorsDock::ClearErrors);

    // Ctrl+C on `body` (not `mList`) so the shortcut still fires
    // after the user clicks Clear (which moves focus off the list).
    auto *copyShortcut = new QShortcut(QKeySequence::Copy, body);
    copyShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copyShortcut, &QShortcut::activated, this, &ParseErrorsDock::CopySelection);

    RefreshSummary();
}

void ParseErrorsDock::AppendErrors(const QString &title, const std::vector<std::string> &errors)
{
    AppendErrorsForSession(nullptr, title, errors);
}

void ParseErrorsDock::AppendErrorsForSession(
    LogSession *originating, const QString &title, const std::vector<std::string> &errors
)
{
    if (errors.empty())
    {
        return;
    }

    // Guard against an empty title rendering as a blank bold row that
    // looks like a missing translation. All in-tree callers pass
    // `tr(...)`; the warning surfaces a future caller that forgets.
    QString effectiveTitle = title;
    if (effectiveTitle.isEmpty())
    {
        qWarning() << "ParseErrorsDock::AppendErrors: empty title; using fallback label";
        effectiveTitle = tr("Errors");
    }

    // A background session receives the batch in its own log only;
    // visible state always belongs to the currently bound session.
    const LogSession *bound = mBoundSession.data();
    if (originating != nullptr && originating != bound)
    {
        AppendErrorsIntoSessionLog(originating, effectiveTitle, errors);
        return;
    }

    const QIcon warningIcon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning);

    const bool firstBatchOfSession = !mHasSeenFirstBatch;

    // Snapshot scroll position so we only re-pin to the tail if the
    // user was already there. Otherwise the new batch lands silently
    // and the user keeps reading whatever rows they scrolled up to.
    const QScrollBar *vBar = mList->verticalScrollBar();
    const bool wasAtTail = vBar == nullptr || (vBar->maximum() - vBar->value()) <= AUTO_FOLLOW_TAIL_SLACK_PX;

    // Strip the existing overflow footer so the new entries land
    // before it; we'll rebuild it at the tail below.
    if (const int last = mList->count() - 1; last >= 0)
    {
        const QListWidgetItem *tail = mList->item(last);
        if (tail != nullptr && tail->data(OVERFLOW_FOOTER_ROLE).toBool())
        {
            delete mList->takeItem(last);
        }
    }

    // Pre-trim a pathologically large batch so we don't mint items
    // we'd immediately delete -- and so the batch's own header isn't
    // evicted along with the surplus rows below it.
    size_t errorsBegin = 0;
    if (errors.size() > static_cast<size_t>(MAX_DISPLAYED_ERRORS))
    {
        errorsBegin = errors.size() - static_cast<size_t>(MAX_DISPLAYED_ERRORS);
        mDroppedCount += static_cast<int>(errorsBegin);
    }

    // Mirror the trimmed batch so bind/unbind need not parse list items.
    MirrorAppendIntoShadow(effectiveTitle, errors, errorsBegin);

    // Group header (disabled so arrow keys step over it).
    auto *headerItem = new QListWidgetItem(effectiveTitle);
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
    TrimShadowToCap();
    // Rebuild the footer here (not inside `TrimToCap`) because a batch
    // sized exactly at the cap pre-trims (bumping `mDroppedCount`)
    // without ever pushing `TrimToCap` past its no-op guard.
    RebuildOverflowFooter();

    if (wasAtTail)
    {
        mList->scrollToBottom();
    }
    RefreshSummary();
    if (firstBatchOfSession)
    {
        // Latch before emitting to guard against a re-entrant
        // `AppendErrors` from a connected slot.
        mHasSeenFirstBatch = true;
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
    mBatchShadow.clear();
    mErrorCount = 0;
    mDroppedCount = 0;
    // `mHasSeenFirstBatch` deliberately NOT reset: re-arming the
    // auto-raise after a user-initiated Clear is the bug we're
    // guarding against. Use `ResetSessionState` for that.
    // Mirror the clear into the bound session so a subsequent
    // rebind does not restore the just-cleared entries.
    if (LogSession *session = mBoundSession.data(); session != nullptr)
    {
        SessionParseErrorLog &log = session->MutableParseErrorLog();
        log.batches.clear();
        log.droppedCount = 0;
        // `hasSeenFirstBatch` stays; same rationale as the local field.
    }
    RefreshSummary();
}

void ParseErrorsDock::ResetSessionState()
{
    // Skip the work in the pristine post-construction state to avoid
    // a spurious `countChanged` emit on every session boundary in
    // idle apps.
    const bool boundSessionHasLog =
        mBoundSession.data() != nullptr &&
        (!mBoundSession->ParseErrorLog().batches.empty() || mBoundSession->ParseErrorLog().droppedCount > 0 ||
         mBoundSession->ParseErrorLog().hasSeenFirstBatch);
    if (mList->count() == 0 && mErrorCount == 0 && mDroppedCount == 0 && !mHasSeenFirstBatch && !boundSessionHasLog)
    {
        return;
    }
    mList->clear();
    mBatchShadow.clear();
    mErrorCount = 0;
    mDroppedCount = 0;
    mHasSeenFirstBatch = false;
    // Reset the bound backing log so cleared batches cannot reappear.
    if (LogSession *session = mBoundSession.data(); session != nullptr)
    {
        session->ResetParseErrorLog();
    }
    RefreshSummary();
}

void ParseErrorsDock::Bind(const SessionBindContext &context)
{
    LogSession *outgoing = mBoundSession.data();
    LogSession *incoming = context.session.data();

    // A live same-session bind is a no-op. Null equality must still
    // clear visible state after QPointer auto-null teardown.
    const bool sameSession = outgoing != nullptr && outgoing == incoming;
    if (sameSession)
    {
        return;
    }

    // Save-outgoing: capture the current visible state into the
    // outgoing session's log (only when outgoing is still alive;
    // a destroyed session's storage is gone).
    if (outgoing != nullptr)
    {
        SessionParseErrorLog &log = outgoing->MutableParseErrorLog();
        log.batches = mBatchShadow;
        log.droppedCount = mDroppedCount;
        log.hasSeenFirstBatch = mHasSeenFirstBatch;
    }

    // Clear the visible state before installing the new session's
    // pointer so any re-entrant slot triggered by `countChanged`
    // observes the unbound state.
    mList->clear();
    mBatchShadow.clear();
    mErrorCount = 0;
    mDroppedCount = 0;
    mHasSeenFirstBatch = false;

    mBoundSession = incoming;

    // Load-incoming: replay the incoming session's log so the
    // visible list matches what the user last saw for this tab.
    // For an unbound context (incoming == null) the clear above
    // is the terminal state.
    if (incoming != nullptr)
    {
        ReplayLogIntoVisibleList(incoming->ParseErrorLog());
    }

    RefreshSummary();
}

void ParseErrorsDock::Unbind()
{
    // Equivalent to Bind(MakeUnbound()) with fewer branches for
    // teardown call sites that already know they want the empty
    // state.
    if (LogSession *session = mBoundSession.data(); session != nullptr)
    {
        SessionParseErrorLog &log = session->MutableParseErrorLog();
        log.batches = std::move(mBatchShadow);
        log.droppedCount = mDroppedCount;
        log.hasSeenFirstBatch = mHasSeenFirstBatch;
    }
    mBoundSession = nullptr;
    mBatchShadow.clear();
    mList->clear();
    mErrorCount = 0;
    mDroppedCount = 0;
    mHasSeenFirstBatch = false;
    RefreshSummary();
}

void ParseErrorsDock::MirrorAppendIntoShadow(
    const QString &title, const std::vector<std::string> &errors, size_t errorsBegin
)
{
    SessionParseErrorBatch batch;
    batch.title = title;
    batch.errors.reserve(errors.size() - errorsBegin);
    for (size_t i = errorsBegin; i < errors.size(); ++i)
    {
        batch.errors.push_back(errors[i]);
    }
    mBatchShadow.push_back(std::move(batch));
}

void ParseErrorsDock::AppendErrorsIntoSessionLog(
    LogSession *session, const QString &title, const std::vector<std::string> &errors
)
{
    if (session == nullptr || errors.empty())
    {
        return;
    }
    SessionParseErrorLog &log = session->MutableParseErrorLog();

    // Pre-trim a pathologically large batch so the resulting log
    // doesn't blow through `MAX_DISPLAYED_ERRORS` in a single
    // append. Matches the pre-trim in the visible path so a
    // subsequent Bind sees a log with the same shape.
    size_t errorsBegin = 0;
    if (errors.size() > static_cast<size_t>(MAX_DISPLAYED_ERRORS))
    {
        errorsBegin = errors.size() - static_cast<size_t>(MAX_DISPLAYED_ERRORS);
        log.droppedCount += static_cast<int>(errorsBegin);
    }

    SessionParseErrorBatch batch;
    batch.title = title;
    batch.errors.reserve(errors.size() - errorsBegin);
    for (size_t i = errorsBegin; i < errors.size(); ++i)
    {
        batch.errors.push_back(errors[i]);
    }
    log.batches.push_back(std::move(batch));

    // FIFO evict from the head until the total is back under the
    // cap. Mirrors `TrimShadowToCap` / `TrimToCap` semantics; the
    // visible-path Bump on `mDroppedCount` is instead written to
    // `log.droppedCount` directly.
    int totalErrors = 0;
    for (const auto &b : log.batches)
    {
        totalErrors += static_cast<int>(b.errors.size());
    }
    while (totalErrors > MAX_DISPLAYED_ERRORS && !log.batches.empty())
    {
        auto &head = log.batches.front();
        const int available = static_cast<int>(head.errors.size());
        const int excess = totalErrors - MAX_DISPLAYED_ERRORS;
        if (excess >= available)
        {
            totalErrors -= available;
            log.droppedCount += available;
            log.batches.erase(log.batches.begin());
        }
        else
        {
            head.errors.erase(head.errors.begin(), head.errors.begin() + excess);
            log.droppedCount += excess;
            totalErrors -= excess;
        }
    }
    // Drop any leading empty batches to match the visible path's
    // orphan-header compaction.
    while (!log.batches.empty() && log.batches.front().errors.empty())
    {
        log.batches.erase(log.batches.begin());
    }

    // First-batch latch: mirror the visible path so the eventual
    // Bind's replay does not re-raise the dock for a background
    // session that has already had its first batch appended.
    log.hasSeenFirstBatch = true;
}

void ParseErrorsDock::TrimShadowToCap()
{
    // Mirror `TrimToCap`'s FIFO eviction: drop entries from the
    // head of the shadow until the total shadowed error count is
    // back under `MAX_DISPLAYED_ERRORS`. Empty batches are dropped
    // outright so a fully-evicted batch does not leave an orphaned
    // header behind on rebind. `mDroppedCount` is already updated
    // by the sibling call in `TrimToCap` -- do not touch it here.
    int totalErrors = 0;
    for (const auto &batch : mBatchShadow)
    {
        totalErrors += static_cast<int>(batch.errors.size());
    }
    while (totalErrors > MAX_DISPLAYED_ERRORS && !mBatchShadow.empty())
    {
        auto &head = mBatchShadow.front();
        const int available = static_cast<int>(head.errors.size());
        const int excess = totalErrors - MAX_DISPLAYED_ERRORS;
        if (excess >= available)
        {
            totalErrors -= available;
            mBatchShadow.erase(mBatchShadow.begin());
        }
        else
        {
            head.errors.erase(head.errors.begin(), head.errors.begin() + excess);
            totalErrors -= excess;
        }
    }
    // Drop any leading batches whose errors are now empty; the
    // visible list's `TrimToCap` compact-orphan-headers pass does
    // the equivalent for `mList`.
    while (!mBatchShadow.empty() && mBatchShadow.front().errors.empty())
    {
        mBatchShadow.erase(mBatchShadow.begin());
    }
}

void ParseErrorsDock::ReplayLogIntoVisibleList(const SessionParseErrorLog &log)
{
    // Seed the running counters + first-batch latch from the log
    // BEFORE the item mint below so `RefreshSummary` (called by
    // the caller after this returns) sees the correct totals in
    // one shot. The trimmed-log semantics mean the replay does
    // not need to re-run TrimToCap here: `AppendErrorsIntoSession
    // Log` / `AppendErrors` already trimmed on the way in.
    mDroppedCount = log.droppedCount;
    mHasSeenFirstBatch = log.hasSeenFirstBatch;
    for (const auto &batch : log.batches)
    {
        // Do NOT re-mirror through `AppendErrors` because that
        // path would re-trigger `firstBatchArrived`, incorrectly
        // re-arm the counters, and mutate `mDroppedCount` a
        // second time. Inline the item-mint + shadow-append.
        const QIcon warningIcon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning);
        auto *headerItem = new QListWidgetItem(batch.title);
        QFont headerFont = headerItem->font();
        headerFont.setBold(true);
        headerItem->setFont(headerFont);
        headerItem->setFlags(Qt::ItemIsEnabled);
        mList->addItem(headerItem);
        for (const auto &error : batch.errors)
        {
            auto *item = new QListWidgetItem(warningIcon, QString::fromStdString(error));
            item->setToolTip(QString::fromStdString(error));
            item->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled);
            mList->addItem(item);
            ++mErrorCount;
        }
        mBatchShadow.push_back(batch);
    }
    // Restore the overflow footer if the incoming log carried a
    // dropped count.
    RebuildOverflowFooter();
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
    // Walk from the top and evict until back under the cap. Group
    // headers don't count toward `mErrorCount` and ride along. We
    // stash the most recently evicted header so a partially-evicted
    // batch's survivors don't end up stranded below the next batch's
    // title.
    QString lastEvictedHeaderText;
    QFont lastEvictedHeaderFont;
    bool haveEvictedHeader = false;
    while (mList->count() > 0 && mErrorCount > MAX_DISPLAYED_ERRORS)
    {
        const QListWidgetItem *item = mList->takeItem(0);
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
            lastEvictedHeaderText = item->text();
            lastEvictedHeaderFont = item->font();
            haveEvictedHeader = true;
        }
        delete item;
    }

    // Re-mint the evicted header if its first surviving row is now
    // stranded at the top with no preceding header (otherwise the
    // user would see a wall of errors that look like they belong to
    // the next batch's header below them).
    if (haveEvictedHeader && mList->count() > 0)
    {
        const QListWidgetItem *first = mList->item(0);
        if (first != nullptr && first->flags().testFlag(Qt::ItemIsSelectable))
        {
            auto *replacement = new QListWidgetItem(lastEvictedHeaderText);
            replacement->setFont(lastEvictedHeaderFont);
            replacement->setFlags(Qt::ItemIsEnabled);
            mList->insertItem(0, replacement);
        }
    }

    // Compact orphan headers at the front: a header followed by
    // another header (or by nothing) lost its entire batch.
    while (mList->count() > 0)
    {
        const QListWidgetItem *first = mList->item(0);
        if (first == nullptr || first->flags().testFlag(Qt::ItemIsSelectable))
        {
            break;
        }
        // First item is a header. Look at its successor.
        const QListWidgetItem *second = mList->count() > 1 ? mList->item(1) : nullptr;
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
    // Caller must strip any prior footer first; assert catches a
    // future refactor that would otherwise stack duplicates.
    Q_ASSERT(mList->count() == 0 || !mList->item(mList->count() - 1)->data(OVERFLOW_FOOTER_ROLE).toBool());
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

    // Walk the list once and synthesise the header above the first
    // selected row in each group, plus the overflow footer at the
    // bottom, so the pasted block reads as self-contained text.
    // Headers / footer are non-selectable, so the user never selects
    // them directly.
    QStringList lines;
    const int count = mList->count();
    const QListWidgetItem *currentHeader = nullptr;
    bool currentHeaderEmitted = false;
    bool sawAnyError = false;
    for (int i = 0; i < count; ++i)
    {
        const QListWidgetItem *item = mList->item(i);
        if (item == nullptr)
        {
            continue;
        }
        const bool isSelectable = item->flags().testFlag(Qt::ItemIsSelectable);
        if (!isSelectable)
        {
            // Either a group header (reset per-group tracking) or
            // the overflow footer (handled after the loop).
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
        // Append the overflow footer so the dropped count travels
        // with the copied excerpt.
        for (int i = count - 1; i >= 0; --i)
        {
            const QListWidgetItem *item = mList->item(i);
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
        // Two independent counts -> can't use a single `%Ln` plural;
        // format both via QLocale for matching digit grouping.
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
