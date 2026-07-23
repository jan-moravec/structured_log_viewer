#include "anchors_dock.hpp"

#include "log_model.hpp"
#include "theme_control.hpp"

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QCloseEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeySequence>
#include <QMenu>
#include <QModelIndex>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPoint>
#include <QPushButton>
#include <QRectF>
#include <QScopeGuard>
#include <QShortcut>
#include <QStringBuilder>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QStyleOptionViewItem>
#include <QTreeWidget>
#include <QTreeWidgetItem>
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

/// Column indices for the two-column tree. Kept as named constants
/// so the `itemChanged` handler and inline-edit code can key off
/// them without magic numbers.
constexpr int COLUMN_ANCHOR = 0;
constexpr int COLUMN_NOTE = 1;

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

/// Adapt `AnchorManager::SanitiseNote` (which is `std::string`-based
/// so it can be called from non-Qt code paths) to the `QString`
/// currency of the widget layer. Round-trip through UTF-8 keeps
/// non-ASCII notes intact.
[[nodiscard]] QString SanitiseNote(const QString &raw)
{
    return QString::fromStdString(AnchorManager::SanitiseNote(raw.toStdString()));
}

/// Delegate installed on `COLUMN_ANCHOR` to hard-refuse editor
/// creation. `Qt::ItemIsEditable` is an item-wide flag on
/// `QTreeWidgetItem` (there's no per-column flag API), so without
/// this delegate a user pressing F2 while focused on the anchor
/// column would open an editor for the "line N - file.json" label
/// and let them garble the display until the next refresh.
///
/// Painting and size-hint stay on `QStyledItemDelegate` defaults --
/// only editor construction is blocked.
class NoEditDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QWidget *createEditor(QWidget * /*parent*/, const QStyleOptionViewItem & /*option*/, const QModelIndex & /*index*/)
        const override
    {
        return nullptr;
    }
};

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

    mTree = new QTreeWidget(host);
    mTree->setObjectName(QStringLiteral("anchorsList"));
    mTree->setColumnCount(2);
    mTree->setHeaderLabels({QObject::tr("Anchor"), QObject::tr("Note")});
    mTree->setRootIsDecorated(false);
    mTree->setUniformRowHeights(true);
    mTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mTree->setContextMenuPolicy(Qt::CustomContextMenu);
    // Inline note editing: double-click on the note column or press
    // F2. Anchor column stays activate-to-jump. The per-column
    // `NoEditDelegate` below blocks editor construction on
    // `COLUMN_ANCHOR` so `F2` with focus there is a no-op instead
    // of opening an editor for the display label.
    mTree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    // `NoEditDelegate` is parented to the tree so it lives exactly
    // as long as the column it guards; no manual delete needed.
    mTree->setItemDelegateForColumn(COLUMN_ANCHOR, new NoEditDelegate(mTree));
    // Note column carries most of the content; give it stretch so
    // long notes don't clip.
    if (auto *headerView = mTree->header(); headerView != nullptr)
    {
        headerView->setSectionResizeMode(COLUMN_ANCHOR, QHeaderView::ResizeToContents);
        headerView->setSectionResizeMode(COLUMN_NOTE, QHeaderView::Stretch);
        headerView->setStretchLastSection(false);
    }
    layout->addWidget(mTree, 1);

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
    // `itemDoubleClicked` too would fire jumps twice per click. Only
    // the anchor column triggers a jump; double-clicking the note
    // starts inline editing (Qt handles the edit path via
    // `editTriggers`).
    connect(mTree, &QTreeWidget::itemActivated, this, &AnchorsDock::OnItemActivated);
    connect(mTree, &QTreeWidget::itemChanged, this, &AnchorsDock::OnItemChanged);
    connect(mTree, &QWidget::customContextMenuRequested, this, &AnchorsDock::OnContextMenuRequested);
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
    if (mTree == nullptr)
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
    if (const QTreeWidgetItem *focused = mTree->currentItem(); focused != nullptr)
    {
        focusedKey.locator = focused->data(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE).toString();
        focusedKey.lineId = focused->data(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE).toULongLong();
        hadFocus = true;
    }
    std::vector<AnchorKeyCarrier> selectedKeys;
    const auto selectedItems = mTree->selectedItems();
    selectedKeys.reserve(static_cast<std::size_t>(selectedItems.size()));
    for (const QTreeWidgetItem *selected : selectedItems)
    {
        selectedKeys.push_back(
            AnchorKeyCarrier{
                .locator = selected->data(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE).toString(),
                .lineId = selected->data(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE).toULongLong(),
            }
        );
    }

    // Suppress `itemChanged` fired by `clear()` + `setText()` during
    // repopulate; otherwise we'd re-enter `SetAnchorNote` on every
    // row we build. Counter (not bool) survives nested emits; the
    // scope guard makes the balance exception-safe so an allocator
    // failure inside the loop can't leave the dock permanently
    // muted.
    ++mSuppressItemChanged;
    const auto suppressGuard = qScopeGuard([this] { --mSuppressItemChanged; });
    mTree->clear();
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
        const QString noteText = QString::fromStdString(entry.note);

        auto *item = new QTreeWidgetItem(mTree);
        item->setIcon(COLUMN_ANCHOR, SwatchIconFor(mTheme.data(), entry.colorIndex, swatchPx));
        item->setText(COLUMN_ANCHOR, label);
        item->setText(COLUMN_NOTE, noteText);
        item->setData(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE, QString::fromStdString(entry.locator));
        item->setData(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE, QVariant::fromValue<qulonglong>(entry.lineId));

        // `ItemIsEditable` is item-wide (Qt has no per-column flag
        // API on `QTreeWidgetItem`); per-column editability is
        // enforced by the `NoEditDelegate` installed on
        // `COLUMN_ANCHOR`, which refuses to build an editor.
        // `OnItemChanged` also drops any `COLUMN_ANCHOR` mutation as
        // a belt-and-braces guard.
        item->setFlags(item->flags() | Qt::ItemIsEditable);

        // Qt tooltips treat their text as HTML when
        // `Qt::mightBeRichText()` matches. User notes and file
        // paths can carry `<`, `&`, and friends, so escape both
        // before interpolation to keep the tooltip literal.
        const QString escapedDisplayPath = displayPath.toHtmlEscaped();
        const QString escapedNote = noteText.toHtmlEscaped();
        const QString tooltipBody = escapedDisplayPath.isEmpty()
                                        ? QObject::tr("Anchor #%1, line %2").arg(entry.colorIndex + 1).arg(entry.lineId)
                                        : QObject::tr("Anchor #%1, line %2\n%3")
                                              .arg(entry.colorIndex + 1)
                                              .arg(entry.lineId)
                                              .arg(escapedDisplayPath);
        const QString tooltipWithNote = escapedNote.isEmpty()
                                            ? tooltipBody
                                            : QObject::tr("%1\nNote: %2").arg(tooltipBody, escapedNote);
        item->setToolTip(COLUMN_ANCHOR, tooltipWithNote);
        item->setToolTip(COLUMN_NOTE, tooltipWithNote);
    }

    if (mClearAllButton != nullptr)
    {
        mClearAllButton->setEnabled(!mAnchors->Empty());
    }

    // Restore selection + focus in one pass so observers see a
    // single selection-changed signal. Vanished items are dropped.
    if (hadFocus || !selectedKeys.empty())
    {
        const QSignalBlocker selectionBlocker(mTree);
        for (int row = 0; row < mTree->topLevelItemCount(); ++row)
        {
            QTreeWidgetItem *item = mTree->topLevelItem(row);
            if (item == nullptr)
            {
                continue;
            }
            const QString itemLocator = item->data(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE).toString();
            const qulonglong itemLineId = item->data(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE).toULongLong();
            const bool itemWasSelected = std::ranges::any_of(selectedKeys, [&](const AnchorKeyCarrier &k) {
                return k.locator == itemLocator && k.lineId == itemLineId;
            });
            if (itemWasSelected)
            {
                item->setSelected(true);
            }
            if (hadFocus && itemLocator == focusedKey.locator && itemLineId == focusedKey.lineId)
            {
                mTree->setCurrentItem(item, COLUMN_ANCHOR, QItemSelectionModel::NoUpdate);
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

int AnchorsDock::SourceRowForItem(const QTreeWidgetItem *item) const
{
    if (item == nullptr || mModel.isNull())
    {
        return -1;
    }
    const AnchorManager::Key key{
        .locator = item->data(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE).toString().toStdString(),
        .lineId = item->data(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE).toULongLong(),
    };
    return mModel->SourceRowForAnchorKey(key);
}

void AnchorsDock::OnItemActivated(QTreeWidgetItem *item, int column)
{
    // Only the anchor column jumps. Activating the note column via
    // Enter is reserved for "commit edit" (Qt handles that itself).
    if (column != COLUMN_ANCHOR)
    {
        return;
    }
    emit jumpToAnchorRequested(SourceRowForItem(item));
}

void AnchorsDock::OnItemChanged(QTreeWidgetItem *item, int column)
{
    if (mSuppressItemChanged > 0 || item == nullptr || mAnchors.isNull())
    {
        return;
    }
    // Only the note column is editable; drop mutations on column 0
    // defensively (belt-and-braces even though the anchor column
    // isn't user-editable through the tree's edit triggers).
    if (column != COLUMN_NOTE)
    {
        return;
    }
    const AnchorManager::Key key{
        .locator = item->data(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE).toString().toStdString(),
        .lineId = item->data(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE).toULongLong(),
    };
    const QString sanitised = SanitiseNote(item->text(COLUMN_NOTE));
    if (sanitised != item->text(COLUMN_NOTE))
    {
        // The user typed a newline or tab; reflect the sanitised
        // form back into the item text so the display matches what
        // we're about to persist. Guard against re-entry into this
        // slot by bumping the suppress counter under a scope guard
        // so an exception from Qt's paint stack can't wedge the
        // counter high.
        ++mSuppressItemChanged;
        const auto suppressGuard = qScopeGuard([this] { --mSuppressItemChanged; });
        item->setText(COLUMN_NOTE, sanitised);
    }
    mAnchors->SetAnchorNote(key, sanitised.toStdString());
    // `AnchorManager::SetAnchorNote` emits `anchorChanged`; the
    // dock refreshes via that signal, which will restore selection
    // + focus around the edited item.
}

void AnchorsDock::OnContextMenuRequested(const QPoint &pos)
{
    if (mTree == nullptr || mAnchors.isNull())
    {
        return;
    }
    QTreeWidgetItem *item = mTree->itemAt(pos);
    if (item == nullptr)
    {
        return;
    }

    // Copy the key out before `exec()` pumps events: a queued anchor
    // signal could `Refresh()` and tear `item` down underneath us.
    // The source row is intentionally resolved *after* `exec()`
    // since a mid-popup eviction would have shifted indices.
    const AnchorManager::Key key{
        .locator = item->data(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE).toString().toStdString(),
        .lineId = item->data(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE).toULongLong(),
    };

    QMenu menu(this);
    const QAction *jumpAction = menu.addAction(QObject::tr("Jump to anchor"));
    // Edit note: the F2 shortcut lives on the tree's edit triggers,
    // not on the menu item -- popup shortcuts wouldn't do anything
    // useful since the menu is already the focus target.
    const QAction *editNoteAction = menu.addAction(QObject::tr("Edit note"));
    const QAction *removeAction = menu.addAction(QObject::tr("Remove anchor"));
    const QAction *picked = menu.exec(mTree->viewport()->mapToGlobal(pos));
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
    if (picked == editNoteAction)
    {
        // Re-find the item because `exec()` may have run a refresh.
        for (int row = 0; row < mTree->topLevelItemCount(); ++row)
        {
            QTreeWidgetItem *candidate = mTree->topLevelItem(row);
            if (candidate == nullptr)
            {
                continue;
            }
            const auto candidateLocator = candidate->data(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE).toString();
            const auto candidateLineId = candidate->data(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE).toULongLong();
            if (candidateLocator.toStdString() == key.locator && candidateLineId == key.lineId)
            {
                mTree->setCurrentItem(candidate, COLUMN_NOTE);
                mTree->editItem(candidate, COLUMN_NOTE);
                break;
            }
        }
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

#ifdef LOGAPP_BUILD_TESTING
void AnchorsDock::BeginEditNoteForTest()
{
    if (mTree == nullptr)
    {
        return;
    }
    QTreeWidgetItem *item = mTree->currentItem();
    if (item == nullptr)
    {
        return;
    }
    mTree->editItem(item, COLUMN_NOTE);
}
#endif
