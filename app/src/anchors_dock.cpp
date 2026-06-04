#include "anchors_dock.hpp"

#include "log_model.hpp"
#include "theme_control.hpp"

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QHBoxLayout>
#include <QIcon>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPoint>
#include <QPushButton>
#include <QRectF>
#include <QStringBuilder>
#include <QStyle>
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace
{

constexpr qreal SWATCH_PAINT_INSET = 0.5;
constexpr qreal SWATCH_CORNER_RADIUS = 3.0;
/// Fallback swatch edge length in device-independent pixels for the
/// rare case where no `QStyle` is available (headless test harness,
/// `QApplication` torn down). Matches the previous hard-coded value.
constexpr int SWATCH_ICON_FALLBACK_PX = 14;

[[nodiscard]] int SwatchIconPixels(const QWidget *widget)
{
    if (const QStyle *style = (widget != nullptr) ? widget->style() : QApplication::style(); style != nullptr)
    {
        const int metric = style->pixelMetric(QStyle::PM_SmallIconSize, nullptr, widget);
        if (metric > 0)
        {
            return metric;
        }
    }
    return SWATCH_ICON_FALLBACK_PX;
}

/// User-role payload carried by every `QListWidgetItem`: the
/// `(locator, lineId)` key that uniquely identifies the anchored
/// row in the live `LogModel`. Stored as a `QVariantMap` for
/// QVariant round-tripping.
constexpr int ANCHOR_KEY_LOCATOR_ROLE = Qt::UserRole + 1;
constexpr int ANCHOR_KEY_LINE_ID_ROLE = Qt::UserRole + 2;

[[nodiscard]] QIcon SwatchIconFor(ThemeControl *theme, std::uint8_t colorIndex, int sizePx)
{
    if (theme == nullptr)
    {
        return QIcon{};
    }
    const QBrush bg = theme->AnchorBrushFor(colorIndex, Qt::BackgroundRole);
    const QBrush fg = theme->AnchorBrushFor(colorIndex, Qt::ForegroundRole);
    QPixmap pix(sizePx, sizePx);
    pix.fill(Qt::transparent);
    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(bg);
    painter.setPen(QPen(fg.color(), 1));
    painter.drawRoundedRect(
        QRectF(SWATCH_PAINT_INSET, SWATCH_PAINT_INSET, sizePx - 1, sizePx - 1),
        SWATCH_CORNER_RADIUS,
        SWATCH_CORNER_RADIUS
    );
    return QIcon{pix};
}

[[nodiscard]] QString FilenameFromLocator(const std::string &locator)
{
    if (locator.empty())
    {
        return {};
    }
    try
    {
        const std::filesystem::path p(locator);
        const std::string filename = p.filename().string();
        return QString::fromStdString(filename.empty() ? locator : filename);
    }
    catch (const std::exception &)
    {
        return QString::fromStdString(locator);
    }
}

/// Resolve @p locator (a `locatorDedupKey`, lowercased on Windows)
/// back to the display-case path the user opened, by consulting the
/// model's `Source::locators` / `locatorDedupKeys` parallel arrays.
/// Falls back to @p locator on a miss -- a multi-file session can
/// carry anchors from a previously-saved source whose locator list
/// the current session no longer mirrors.
[[nodiscard]] QString DisplayPathForLocator(const LogModel *model, const std::string &locator)
{
    if (model == nullptr || locator.empty())
    {
        return QString::fromStdString(locator);
    }
    const auto &configurationSource = model->Configuration().source;
    if (!configurationSource.has_value())
    {
        return QString::fromStdString(locator);
    }
    const auto &dedupKeys = configurationSource->locatorDedupKeys;
    const auto &displayPaths = configurationSource->locators;
    // `AppendLocator` keeps these arrays in lockstep; if they ever
    // desync we still fall back to the dedup key rather than
    // indexing out of `displayPaths`.
    const std::size_t count = std::min(dedupKeys.size(), displayPaths.size());
    for (std::size_t i = 0; i < count; ++i)
    {
        if (dedupKeys[i] == locator)
        {
            return QString::fromStdString(displayPaths[i]);
        }
    }
    return QString::fromStdString(locator);
}

} // namespace

AnchorsDock::AnchorsDock(AnchorManager *anchors, LogModel *model, ThemeControl *theme, QWidget *parent)
    : QDockWidget(QObject::tr("Anchors"), parent), mAnchors(anchors), mModel(model), mTheme(theme)
{
    setObjectName(QStringLiteral("anchorsDock"));

    auto *host = new QWidget(this);
    auto *layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto *header = new QHBoxLayout();
    mClearAllButton = new QPushButton(QObject::tr("Clear all"), host);
    mClearAllButton->setObjectName(QStringLiteral("anchorsClearAll"));
    // Disabled by default -- `Refresh()` flips it on when at least
    // one anchor exists. Without this initial state the empty-dock
    // case would offer a button that, on click, would be a no-op
    // (AnchorManager::ClearAll bails on an empty map).
    mClearAllButton->setEnabled(false);
    header->addStretch(1);
    header->addWidget(mClearAllButton);
    layout->addLayout(header);

    mList = new QListWidget(host);
    mList->setObjectName(QStringLiteral("anchorsList"));
    mList->setUniformItemSizes(true);
    mList->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mList->setContextMenuPolicy(Qt::CustomContextMenu);
    layout->addWidget(mList, 1);

    setWidget(host);

    // Lambdas call the gated `Refresh()` directly. The early-bail in
    // `Refresh()` (against `IsVisibleForRefresh()`) short-circuits
    // buried-dock cases without each call site repeating the check.
    if (mAnchors != nullptr)
    {
        connect(mAnchors, &AnchorManager::anchorChanged, this, [this](const AnchorManager::Key &) { Refresh(); });
        connect(mAnchors, &AnchorManager::anchorsReset, this, [this]() { Refresh(); });
    }

    // Repopulate when the underlying model's row set changes -- the
    // resolved "filename" column may flip on a streamed batch that
    // promotes a previously-empty locator. The gate inside
    // `Refresh()` ensures a buried dock doesn't pay for every
    // `modelReset` (which fires on every `BeginStreaming`, `Reset`,
    // `NotifyConfigurationReplaced` and reloaded config -- bursts
    // that target the table, not us). Re-shown docks pick up the
    // latest state through the `visibilityChanged` handler below.
    if (mModel != nullptr)
    {
        connect(mModel, &QAbstractItemModel::modelReset, this, [this]() { Refresh(); });
    }

    // Theme switch (`Preferences -> Theme` or OS dark-mode flip in
    // Auto): anchor swatches read from `ThemeControl::AnchorBrushFor`,
    // so without this connect the dock keeps painting the previous
    // theme's palette until the next anchor mutation triggers a
    // refresh. Gated through `Refresh()` so a buried dock skips work.
    if (mTheme != nullptr)
    {
        connect(mTheme, &ThemeControl::themeChanged, this, [this]() { Refresh(); });
    }

    // `itemActivated` fires on Enter/Return on the focused item *and*
    // on double-click for default-styled item views (the
    // `SH_ItemView_ActivateItemOnSingleClick` style hint is off on
    // every desktop platform we ship for). Wiring `itemDoubleClicked`
    // alongside it would emit `jumpToAnchorRequested` twice for one
    // user double-click and re-run `MainWindow::SelectSourceRow`
    // (which re-clears + re-selects + re-scrolls) twice in a row.
    connect(mList, &QListWidget::itemActivated, this, &AnchorsDock::OnItemActivated);
    connect(mList, &QWidget::customContextMenuRequested, this, &AnchorsDock::OnContextMenuRequested);
    connect(mClearAllButton, &QPushButton::clicked, this, &AnchorsDock::OnClearAllClicked);

    // The first `visibilityChanged(true)` flips `mPerceivedVisible`
    // from its false default (see header) and drives the very first
    // population via `RefreshAlways` -- bypassing the gate is safe
    // here because the visibility just opened. Subsequent visibility
    // flips go back through the gated path. We do NOT call
    // `Refresh()` at the end of the constructor: the dock starts
    // hidden so the gate would reject it; an explicit no-op call
    // would just be misleading.
    connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        mPerceivedVisible = visible;
        if (visible)
        {
            RefreshAlways();
        }
    });
}

void AnchorsDock::Refresh()
{
    // Gate: buried docks (hidden window, tabified-but-buried,
    // bare-shell offscreen QPA) skip the full snapshot + repopulate
    // pass. `visibilityChanged(true)` calls `RefreshAlways` directly
    // so the user sees fresh content the moment they re-open the
    // dock. Tests that need to inspect the dock under offscreen QPA
    // (where `visibilityChanged` never fires) go through
    // `RefreshForTest`.
    if (!IsVisibleForRefresh())
    {
        return;
    }
    RefreshAlways();
}

void AnchorsDock::RefreshAlways()
{
    if (mList == nullptr)
    {
        return;
    }
    // Snapshot the user's focus + selection BEFORE `clear()` wipes
    // the list's selection model. Without this, even cheap refreshes
    // (right-click + remove an anchor, theme switch, anchor recolour)
    // pull the highlight off the entry the user was just touching.
    // We snapshot by anchor key rather than by row index because the
    // refresh below recreates every row in a possibly-different
    // order (`Entries()` is sorted, but the previous order is gone
    // by the time we rebuild).
    struct AnchorKeyCarrier
    {
        QString locator;
        qulonglong lineId = 0;
    };
    AnchorKeyCarrier focusedKey;
    bool hadFocus = false;
    if (const QListWidgetItem *focused = mList->currentItem(); focused != nullptr)
    {
        focusedKey.locator = focused->data(ANCHOR_KEY_LOCATOR_ROLE).toString();
        focusedKey.lineId = focused->data(ANCHOR_KEY_LINE_ID_ROLE).toULongLong();
        hadFocus = true;
    }
    std::vector<AnchorKeyCarrier> selectedKeys;
    const auto selectedItems = mList->selectedItems();
    selectedKeys.reserve(static_cast<std::size_t>(selectedItems.size()));
    for (const QListWidgetItem *selected : selectedItems)
    {
        selectedKeys.push_back(AnchorKeyCarrier{
            .locator = selected->data(ANCHOR_KEY_LOCATOR_ROLE).toString(),
            .lineId = selected->data(ANCHOR_KEY_LINE_ID_ROLE).toULongLong(),
        });
    }

    mList->clear();
    if (mAnchors == nullptr)
    {
        return;
    }

    // Resolve the swatch size once per refresh from the active
    // style so the swatches scale with HiDPI / Fusion vs Windows
    // vs macOS look-and-feel. Falls back to a hard-coded value in
    // the unusual case where no `QStyle` is reachable.
    const int swatchPx = SwatchIconPixels(this);

    const auto entries = mAnchors->Entries();
    for (const auto &entry : entries)
    {
        // Prefer the display-case path (`Source::locators[i]`) over
        // the canonicalised locator (`locatorDedupKeys[i]`, lower-
        // cased on Windows) so the dock label matches what the user
        // sees in the title bar / open-recents menu. The dedup key
        // still flows through `ANCHOR_KEY_LOCATOR_ROLE` for the
        // SourceRowForAnchorKey lookup.
        const QString displayPath = DisplayPathForLocator(mModel.data(), entry.locator);
        const QString filename = FilenameFromLocator(displayPath.toStdString());
        const QString label = filename.isEmpty() ? QObject::tr("line %1").arg(entry.lineId)
                                                 : QObject::tr("line %1 - %2").arg(entry.lineId).arg(filename);

        auto *item = new QListWidgetItem(SwatchIconFor(mTheme.data(), entry.colorIndex, swatchPx), label, mList);
        item->setData(ANCHOR_KEY_LOCATOR_ROLE, QString::fromStdString(entry.locator));
        item->setData(ANCHOR_KEY_LINE_ID_ROLE, QVariant::fromValue<qulonglong>(entry.lineId));
        item->setToolTip(
            displayPath.isEmpty()
                ? QObject::tr("Anchor #%1, line %2").arg(entry.colorIndex + 1).arg(entry.lineId)
                : QObject::tr("Anchor #%1, line %2\n%3").arg(entry.colorIndex + 1).arg(entry.lineId).arg(displayPath)
        );
    }

    // Keep the Clear-all button enabled iff there is something to
    // clear. Mirrors the constructor's "disabled by default" state.
    if (mClearAllButton != nullptr)
    {
        mClearAllButton->setEnabled(!mAnchors->Empty());
    }

    // Restore selection + focus. We do this in a single pass after
    // the items exist so the selection-changed signals fire once
    // (matters for downstream observers that recompute "is the
    // remove-anchor menu enabled" off the dock selection). Items
    // that disappeared in the refresh (e.g. the entry the user
    // just removed) are silently dropped.
    if (hadFocus || !selectedKeys.empty())
    {
        const QSignalBlocker selectionBlocker(mList);
        for (int row = 0; row < mList->count(); ++row)
        {
            QListWidgetItem *item = mList->item(row);
            if (item == nullptr)
            {
                continue;
            }
            const QString itemLocator = item->data(ANCHOR_KEY_LOCATOR_ROLE).toString();
            const qulonglong itemLineId = item->data(ANCHOR_KEY_LINE_ID_ROLE).toULongLong();
            const bool itemWasSelected = std::ranges::any_of(selectedKeys, [&](const AnchorKeyCarrier &k) {
                return k.locator == itemLocator && k.lineId == itemLineId;
            });
            if (itemWasSelected)
            {
                item->setSelected(true);
            }
            if (hadFocus && itemLocator == focusedKey.locator && itemLineId == focusedKey.lineId)
            {
                mList->setCurrentItem(item, QItemSelectionModel::NoUpdate);
            }
        }
    }
}

bool AnchorsDock::IsVisibleForRefresh() const noexcept
{
    if (isHidden())
    {
        return false;
    }
    return mPerceivedVisible;
}

int AnchorsDock::SourceRowForItem(const QListWidgetItem *item) const
{
    if (item == nullptr || mModel.isNull())
    {
        return -1;
    }
    const AnchorManager::Key key{
        .locator = item->data(ANCHOR_KEY_LOCATOR_ROLE).toString().toStdString(),
        .lineId = item->data(ANCHOR_KEY_LINE_ID_ROLE).toULongLong(),
    };
    return mModel->SourceRowForAnchorKey(key);
}

void AnchorsDock::OnItemActivated(QListWidgetItem *item)
{
    emit jumpToAnchorRequested(SourceRowForItem(item));
}

void AnchorsDock::OnContextMenuRequested(const QPoint &pos)
{
    if (mList == nullptr || mAnchors.isNull())
    {
        return;
    }
    const QListWidgetItem *item = mList->itemAt(pos);
    if (item == nullptr)
    {
        return;
    }

    // Capture the anchor key BEFORE `menu.exec()` pumps the event
    // loop. While the popup is up any queued anchor signal can land
    // on this thread, fire `Refresh()`, and tear down `item` --
    // accessing it after `exec()` returns would be a use-after-free.
    // The stack-local key survives that re-entry. The source row is
    // intentionally NOT captured here: a FIFO eviction mid-popup
    // would shift every surviving row's index, so we re-resolve from
    // the (stable) key after `exec()` returns.
    const AnchorManager::Key key{
        .locator = item->data(ANCHOR_KEY_LOCATOR_ROLE).toString().toStdString(),
        .lineId = item->data(ANCHOR_KEY_LINE_ID_ROLE).toULongLong(),
    };

    QMenu menu(this);
    const QAction *jumpAction = menu.addAction(QObject::tr("Jump to anchor"));
    const QAction *removeAction = menu.addAction(QObject::tr("Remove anchor"));
    const QAction *picked = menu.exec(mList->viewport()->mapToGlobal(pos));
    if (picked == nullptr)
    {
        return;
    }
    if (picked == jumpAction)
    {
        // `menu.exec()` pumped the event loop, so re-check the
        // QPointer the same way the function entry did. Re-resolve
        // the source row from the key here: a streaming eviction
        // between menu-open and click would have shifted every row,
        // and `SourceRowForAnchorKey` returns -1 for an anchor that
        // no longer has a live row -- `MainWindow::SelectSourceRow`
        // handles that case by surfacing a status-bar note.
        if (mModel.isNull())
        {
            return;
        }
        emit jumpToAnchorRequested(mModel->SourceRowForAnchorKey(key));
        return;
    }
    if (picked == removeAction)
    {
        // `menu.exec()` pumped the event loop, so re-check the
        // QPointer the same way the function entry did -- the
        // anchor manager could have been torn down while the popup
        // was up. Matches the stack-local-capture rationale above.
        if (mAnchors.isNull())
        {
            return;
        }
        mAnchors->RemoveAnchor(key);
    }
}

void AnchorsDock::OnClearAllClicked()
{
    if (!mAnchors.isNull())
    {
        mAnchors->ClearAll();
    }
}
