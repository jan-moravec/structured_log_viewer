#include "record_detail_dock.hpp"

#include "anchor_manager.hpp"
#include "log_model.hpp"
#include "log_session.hpp"
#include "log_session_presentation.hpp"
#include "record_detail_widget.hpp"
#include "session_bind_context.hpp"

#include <QAbstractItemModel>
#include <QCloseEvent>
#include <QList>
#include <QModelIndex>
#include <QObject>

namespace
{
/// Minimum width for side-docked layout. Keeps the field/value table
/// readable without squeezing the central log view too much.
constexpr int DOCK_MIN_WIDTH = 280;
} // namespace

RecordDetailDock::RecordDetailDock(LogModel *model, AnchorManager *anchors, QWidget *parent)
    : QDockWidget(tr("Record Details"), parent), mModel(model), mAnchors(anchors)
{
    setObjectName(QStringLiteral("recordDetailDock"));
    setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea | Qt::BottomDockWidgetArea);
    setFeatures(QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable | QDockWidget::DockWidgetFloatable);

    mWidget = new RecordDetailWidget(this);
    setWidget(mWidget);
    setMinimumWidth(DOCK_MIN_WIDTH);

    // Side-dock width floor only. When docked at the bottom the
    // field/value table can wrap freely, so a narrow horizontal slot
    // is more useful than a hard minimum.
    connect(this, &QDockWidget::dockLocationChanged, this, [this](Qt::DockWidgetArea area) {
        setMinimumWidth(area == Qt::BottomDockWidgetArea ? 0 : DOCK_MIN_WIDTH);
    });

    connect(
        mWidget, &RecordDetailWidget::openInNewWindowRequested, this, &RecordDetailDock::OnOpenInNewWindowRequested
    );

    // Track "is the user actually seeing this pane". `isHidden()`
    // alone misses tabified-dock cases where our tab is buried but
    // the explicit-hide flag is still false. On resume, refresh once
    // to catch up on changes we ignored while invisible.
    connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        const bool wasVisible = mPerceivedVisible;
        mPerceivedVisible = visible;
        if (visible && !wasVisible && mCurrentSourceIndex.isValid())
        {
            RefreshFromModel();
        }
    });

    InstallSourceSubscriptions();

    Clear();
}

void RecordDetailDock::InstallSourceSubscriptions()
{
    if (mModel != nullptr)
    {
        // `modelReset` (file open, `LogModel::Reset`) drops every row
        // without firing `rowsRemoved`. Listening here keeps the dock
        // self-contained -- callers don't have to wire it.
        mSourceConnections += connect(mModel, &QAbstractItemModel::modelReset, this, &RecordDetailDock::Clear);
        mSourceConnections +=
            connect(mModel, &QAbstractItemModel::rowsRemoved, this, [this](const QModelIndex &, int, int) {
                if (mCurrentSourceIndex.isValid())
                {
                    // Pinned row survives, only its index shifted; refresh
                    // so the summary's row number is current.
                    if (!IsVisibleForRefresh())
                    {
                        return;
                    }
                    RefreshFromModel();
                }
                else if (mEverPinned)
                {
                    // Pinned row was just evicted. Always swap to the
                    // dedicated placeholder (cheap; no field rebuild) so
                    // the next show surfaces "record is gone" instead of
                    // the default "select a row" prompt.
                    ShowEvictedPlaceholder();
                }
                // else: never pinned -- nothing to invalidate.
            });
        // Pinned-row edits (back-fill, header rename, enum promotion)
        // emit `dataChanged` covering the row. Without this the pane
        // would show stale values until the user re-selects the row.
        mSourceConnections += connect(
            mModel,
            &QAbstractItemModel::dataChanged,
            this,
            [this](const QModelIndex &topLeft, const QModelIndex &bottomRight, const QList<int> &roles) {
                if (!mCurrentSourceIndex.isValid())
                {
                    return;
                }
                if (!IsVisibleForRefresh())
                {
                    return;
                }
                // Skip style-only emits (theme-switch repaints): the
                // pane renders display-role text, so Background /
                // Foreground / Font flips don't change what we show.
                if (LogModel::IsStyleOnlyRoleChange(roles))
                {
                    return;
                }
                const int pinnedRow = mCurrentSourceIndex.row();
                if (pinnedRow >= topLeft.row() && pinnedRow <= bottomRight.row())
                {
                    RefreshFromModel();
                }
            }
        );
        // Column reorders (header drag, streaming `Time` bubble) and
        // new-key inserts change the order the Field/Value table
        // renders, so rebuild the field list. The pinned row itself
        // is unaffected -- column 0 is never removed.
        auto columnsLayoutChanged = [this]() {
            if (!IsVisibleForRefresh() || !mCurrentSourceIndex.isValid())
            {
                return;
            }
            RefreshFromModel();
        };
        mSourceConnections += connect(mModel, &QAbstractItemModel::columnsMoved, this, columnsLayoutChanged);
        mSourceConnections += connect(mModel, &QAbstractItemModel::columnsInserted, this, columnsLayoutChanged);
    }

    // Subscribe directly to anchor signals so the pinned row's note
    // subline tracks edits from the Anchors dock / F4 shortcut. The
    // model's `dataChanged` emits pass `IsStyleOnlyRoleChange`, so
    // `RefreshFromModel` (raw-line read + JSON pretty-print + cell
    // rebuild) won't fire off them.
    if (mAnchors != nullptr)
    {
        // `anchorChanged`: add / remove flips subline visibility;
        // a pure recolour on an already-anchored row changes
        // nothing the widget renders, so skip the rebuild.
        mSourceConnections +=
            connect(mAnchors, &AnchorManager::anchorChanged, this, [this](const AnchorManager::Key &changedKey) {
                if (!IsVisibleForRefresh() || !mCurrentSourceIndex.isValid() || mModel.isNull())
                {
                    return;
                }
                const int pinnedRow = mCurrentSourceIndex.row();
                const auto pinnedKey = mModel->AnchorKeyForRow(pinnedRow);
                if (!pinnedKey.has_value() || *pinnedKey != changedKey)
                {
                    return;
                }
                const bool wasAnchored = mWidget->Content().anchorColorIndex.has_value();
                const bool nowAnchored = mModel->AnchorSlotForRow(pinnedRow).has_value();
                if (wasAnchored == nowAnchored)
                {
                    return;
                }
                RefreshFromModel();
            });
        // `anchorNoteChanged`: always changes visible text.
        mSourceConnections +=
            connect(mAnchors, &AnchorManager::anchorNoteChanged, this, [this](const AnchorManager::Key &changedKey) {
                if (!IsVisibleForRefresh() || !mCurrentSourceIndex.isValid() || mModel.isNull())
                {
                    return;
                }
                const int pinnedRow = mCurrentSourceIndex.row();
                const auto pinnedKey = mModel->AnchorKeyForRow(pinnedRow);
                if (pinnedKey.has_value() && *pinnedKey == changedKey)
                {
                    RefreshFromModel();
                }
            });
        // Bulk mutation: refresh unconditionally, the pinned row's
        // anchor state may have changed even if the key didn't.
        mSourceConnections += connect(mAnchors, &AnchorManager::anchorsReset, this, [this]() {
            if (!IsVisibleForRefresh() || !mCurrentSourceIndex.isValid())
            {
                return;
            }
            RefreshFromModel();
        });
    }
}

void RecordDetailDock::Bind(const SessionBindContext &context)
{
    // Save first, then invalidate the persistent index and disconnect
    // before replacing its model. Restore only after new subscriptions
    // and the incoming session alias are installed.

    // Session source objects are stable for a session's lifetime,
    // so a live same-session bind preserves the persistent index.
    LogSession *incoming = context.session.data();
    if (!mBoundSession.isNull() && mBoundSession.data() == incoming)
    {
        return;
    }

    SaveStateIntoBoundSession();

    mCurrentSourceIndex = QPersistentModelIndex();
    mEverPinned = false;

    mSourceConnections.Clear();

    mModel = context.model.data();
    mAnchors = context.anchors.data();

    InstallSourceSubscriptions();

    mBoundSession = incoming;

    RestoreStateFromSession(mBoundSession.data());
}

void RecordDetailDock::Unbind()
{
    Bind(SessionBindContext::MakeUnbound());
}

void RecordDetailDock::SaveStateIntoBoundSession()
{
    if (mBoundSession.isNull())
    {
        return;
    }
    auto &pin = mBoundSession->MutableRecordDetailPin();
    const int currentRow = CurrentSourceRow();
    pin.pinnedSourceRow = currentRow;
    pin.everPinned = mEverPinned;
    // Persist a stable identity so leading-row eviction cannot retarget
    // the saved numeric row to a different record.
    pin.keyLocator.clear();
    pin.keyLineId = 0;
    if (currentRow >= 0 && mModel != nullptr)
    {
        if (const auto key = mModel->AnchorKeyForRow(currentRow); key.has_value())
        {
            pin.keyLocator = key->locator;
            pin.keyLineId = key->lineId;
        }
    }
}

void RecordDetailDock::RestoreStateFromSession(LogSession *session)
{
    if (session == nullptr)
    {
        // Fully unbound: show the default placeholder (no session
        // context means no possible row to restore, and the
        // "select a row" prompt reads better than the evicted
        // variant).
        Clear();
        return;
    }
    const auto &pin = session->RecordDetailPin();

    // Prefer stable identity over the numeric row, which can shift
    // after leading-row eviction. Fall back to the saved row.
    if (mModel != nullptr && !pin.keyLocator.empty() && pin.keyLineId != 0)
    {
        AnchorManager::Key key;
        key.locator = pin.keyLocator;
        key.lineId = pin.keyLineId;
        const int resolvedRow = mModel->SourceRowForAnchorKey(key);
        if (resolvedRow >= 0)
        {
            ShowSourceRow(resolvedRow);
            return;
        }
        // Key did not resolve. If the fallback row number is also
        // gone, fall through to the eviction placeholder below;
        // otherwise the row-number branch will show the row at
        // the persisted index (best-effort continuity).
    }

    if (pin.pinnedSourceRow >= 0)
    {
        // ShowSourceRow revalidates against the current model.
        ShowSourceRow(pin.pinnedSourceRow);
        return;
    }
    if (pin.everPinned)
    {
        // The session had a row pinned that was later evicted;
        // surface the dedicated placeholder rather than the
        // default so the user sees continuity across the
        // rebind.
        Clear();
        mEverPinned = true;
        ShowEvictedPlaceholder();
        return;
    }
    Clear();
}

int RecordDetailDock::CurrentSourceRow() const noexcept
{
    return mCurrentSourceIndex.isValid() ? mCurrentSourceIndex.row() : -1;
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
    mCurrentSourceIndex = QPersistentModelIndex(mModel->index(sourceRow, 0));
    if (!mCurrentSourceIndex.isValid())
    {
        // Defensive: only happens if a future `LogModel` layout
        // produces zero columns with positive `rowCount`. Avoids
        // latching `mEverPinned` on a useless pin.
        Clear();
        return;
    }
    mEverPinned = true;
    RefreshFromModel();
}

void RecordDetailDock::Clear()
{
    mCurrentSourceIndex = QPersistentModelIndex();
    mEverPinned = false;
    RecordDetailContent placeholder;
    placeholder.valid = false;
    placeholder.placeholderText = DefaultRecordDetailPlaceholder();
    mWidget->SetContent(placeholder);
}

void RecordDetailDock::ShowEvictedPlaceholder()
{
    // Qt invalidated the persistent index during `rowsRemoved`;
    // reset it explicitly so `CurrentSourceRow()` is deterministic.
    mCurrentSourceIndex = QPersistentModelIndex();
    RecordDetailContent placeholder;
    placeholder.valid = false;
    placeholder.placeholderText = EvictedRecordPlaceholder();
    mWidget->SetContent(placeholder);
}

void RecordDetailDock::RefreshFromModel()
{
    if (!mModel || !mCurrentSourceIndex.isValid())
    {
        Clear();
        return;
    }
    mWidget->SetContent(BuildRecordDetailContent(*mModel, mCurrentSourceIndex.row()));
#ifdef LOGAPP_BUILD_TESTING
    ++mRefreshCount;
#endif
}

bool RecordDetailDock::IsVisibleForRefresh() const noexcept
{
    // `mPerceivedVisible` already tracks `visibilityChanged`, which
    // covers tabified docks (Qt fires `visibilityChanged(false)`
    // while `isHidden()` stays false) and lets headless tests drive
    // the gate by emitting the signal directly.
    return mPerceivedVisible;
}

void RecordDetailDock::OnOpenInNewWindowRequested()
{
    // Read the row through the persistent index so eviction shifts
    // are reflected and the snapshot points at the actual record.
    emit openInNewWindowRequested(CurrentSourceRow());
}

void RecordDetailDock::closeEvent(QCloseEvent *event)
{
    QDockWidget::closeEvent(event);
    if (event->isAccepted())
    {
        emit closed();
    }
}
