#include "anchors_dock.hpp"

#include "log_model.hpp"
#include "theme_control.hpp"

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QCloseEvent>
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
/// Swatch edge length when no `QStyle` is reachable (headless tests).
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

/// User-role slots carrying the `(locator, lineId)` key for each item.
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

/// Map @p locator (a canonical `locatorDedupKey`) back to the
/// display-case path. Falls back to @p locator on a miss.
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
    // `min` guards against the two arrays desyncing.
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
    // `Refresh()` enables this once there is something to clear.
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

    // `Refresh()` itself gates on visibility, so every wired path
    // short-circuits cheaply when the dock is buried.
    if (mAnchors != nullptr)
    {
        connect(mAnchors, &AnchorManager::anchorChanged, this, [this](const AnchorManager::Key &) { Refresh(); });
        connect(mAnchors, &AnchorManager::anchorsReset, this, [this]() { Refresh(); });
    }

    // `modelReset` matters: a streamed batch can promote a previously
    // empty locator and change the resolved filename column.
    if (mModel != nullptr)
    {
        connect(mModel, &QAbstractItemModel::modelReset, this, [this]() { Refresh(); });
    }

    // Theme switch repaints all swatches.
    if (mTheme != nullptr)
    {
        connect(mTheme, &ThemeControl::themeChanged, this, [this]() { Refresh(); });
    }

    // `itemActivated` covers both Enter and double-click. Wiring
    // `itemDoubleClicked` too would fire jumps twice per click.
    connect(mList, &QListWidget::itemActivated, this, &AnchorsDock::OnItemActivated);
    connect(mList, &QWidget::customContextMenuRequested, this, &AnchorsDock::OnContextMenuRequested);
    connect(mClearAllButton, &QPushButton::clicked, this, &AnchorsDock::OnClearAllClicked);

    // `RefreshAlways` is safe to bypass the gate here because
    // visibility just opened. Don't call `Refresh()` at construction:
    // the dock starts hidden and the gate would reject it anyway.
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
    // Buried docks skip the rebuild; `visibilityChanged(true)` will
    // drive `RefreshAlways` directly when the user re-opens it.
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
    // Snapshot focus + selection by key so the user's highlight
    // survives the `clear()` + repopulate below (row order may shift).
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
        selectedKeys.push_back(
            AnchorKeyCarrier{
                .locator = selected->data(ANCHOR_KEY_LOCATOR_ROLE).toString(),
                .lineId = selected->data(ANCHOR_KEY_LINE_ID_ROLE).toULongLong(),
            }
        );
    }

    mList->clear();
    if (mAnchors == nullptr)
    {
        return;
    }

    // Resolve once so swatches scale with HiDPI / native style.
    const int swatchPx = SwatchIconPixels(this);

    const auto entries = mAnchors->Entries();
    for (const auto &entry : entries)
    {
        // Show the display-case path; keep the canonical locator in
        // the user-role data for the SourceRowForAnchorKey lookup.
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

    if (mClearAllButton != nullptr)
    {
        mClearAllButton->setEnabled(!mAnchors->Empty());
    }

    // Restore selection + focus in one pass so observers see a
    // single selection-changed signal. Vanished items are dropped.
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

void AnchorsDock::closeEvent(QCloseEvent *event)
{
    QDockWidget::closeEvent(event);
    if (event->isAccepted())
    {
        emit closed();
    }
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

    // Copy the key out before `exec()` pumps events: a queued anchor
    // signal could `Refresh()` and tear `item` down underneath us.
    // The source row is intentionally resolved *after* `exec()`
    // since a mid-popup eviction would have shifted indices.
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
        // Re-check QPointers after the event-loop re-entry.
        if (mModel.isNull())
        {
            return;
        }
        emit jumpToAnchorRequested(mModel->SourceRowForAnchorKey(key));
        return;
    }
    if (picked == removeAction)
    {
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
