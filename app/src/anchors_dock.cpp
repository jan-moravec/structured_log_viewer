#include "anchors_dock.hpp"

#include "log_model.hpp"
#include "theme_control.hpp"

#include <QAction>
#include <QApplication>
#include <QBrush>
#include <QCloseEvent>
#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QKeyEvent>
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
#include <functional>
#include <string>
#include <unordered_set>
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

/// Anchor identity pulled off a `QTreeWidgetItem`'s user-role data.
/// Used by `RefreshAlways` to remember selection + focus across a
/// `clear()` + repopulate. Lives at file scope (not inside the
/// function) because MSVC's `unordered_set` traits reject
/// locally-scoped key types.
struct AnchorKeyCarrier
{
    QString locator;
    qulonglong lineId = 0;

    friend bool operator==(const AnchorKeyCarrier &, const AnchorKeyCarrier &) noexcept = default;
};

struct AnchorKeyCarrierHash
{
    std::size_t operator()(const AnchorKeyCarrier &carrier) const noexcept
    {
        const std::size_t locatorHash = qHash(carrier.locator);
        const std::size_t lineIdHash = std::hash<qulonglong>{}(carrier.lineId);
        // boost::hash_combine mix (same as `AnchorManager::KeyHash`).
        constexpr std::size_t GOLDEN_RATIO_HASH = 0x9E3779B9U;
        constexpr std::size_t LEFT_SHIFT = 6U;
        constexpr std::size_t RIGHT_SHIFT = 2U;
        return locatorHash ^
               (lineIdHash + GOLDEN_RATIO_HASH + (locatorHash << LEFT_SHIFT) + (locatorHash >> RIGHT_SHIFT));
    }
};

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

/// Filename portion of @p displayPath. Uses `QFileInfo` (UTF-16)
/// so non-ASCII paths (`логи/приложение.jsonl`) survive on Windows,
/// where `std::filesystem::path(std::string)` would mangle them via
/// the ANSI codepage.
[[nodiscard]] QString FilenameFromDisplayPath(const QString &displayPath)
{
    if (displayPath.isEmpty())
    {
        return {};
    }
    const QString filename = QFileInfo(displayPath).fileName();
    return filename.isEmpty() ? displayPath : filename;
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

/// QString adapter for `AnchorManager::SanitiseNote`. Round-trips
/// through UTF-8 to keep non-ASCII notes intact.
[[nodiscard]] QString SanitiseNote(const QString &raw)
{
    return QString::fromStdString(AnchorManager::SanitiseNote(raw.toStdString()));
}

/// `<qt>`-wrapped tooltip HTML shared by the full rebuild and the
/// surgical note-refresh paths. Centralised so the escaping and
/// wrapper rules can't drift between the two.
[[nodiscard]] QString BuildAnchorTooltipHtml(
    std::uint8_t colorIndex, std::uint64_t lineId, const QString &displayPath, const QString &noteText
)
{
    const QString escapedDisplayPath = displayPath.toHtmlEscaped();
    const QString escapedNote = noteText.toHtmlEscaped();
    // `AnchorsDock::tr` (not `QObject::tr`) so Linguist groups
    // the dock's user-visible strings together.
    const QString tooltipBody = escapedDisplayPath.isEmpty()
                                    ? AnchorsDock::tr("Anchor #%1, line %2").arg(colorIndex + 1).arg(lineId)
                                    : AnchorsDock::tr("Anchor #%1, line %2<br/>%3")
                                          .arg(colorIndex + 1)
                                          .arg(lineId)
                                          .arg(escapedDisplayPath);
    const QString tooltipBodyWithNote =
        escapedNote.isEmpty() ? tooltipBody : AnchorsDock::tr("%1<br/>Note: %2").arg(tooltipBody, escapedNote);
    return QStringLiteral("<qt>%1</qt>").arg(tooltipBodyWithNote);
}

/// Delegate installed on `COLUMN_ANCHOR` to hard-refuse editor
/// creation. `Qt::ItemIsEditable` is item-wide (no per-column flag
/// on `QTreeWidgetItem`), so without this a user pressing F2 on
/// the anchor column would open an editor for the "line N - file"
/// label. Painting and size-hint stay on the defaults.
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

/// Populate the anchor cell (icon + label + key user-role data)
/// and both column tooltips of @p item from @p entry. The note
/// cell is deliberately NOT touched -- surgical callers may hit
/// items whose note is under active inline edit, and rewriting
/// `COLUMN_NOTE` would clobber the in-flight text. Full-rebuild
/// callers set the note cell separately after this call.
void PopulateAnchorCellForEntry(
    QTreeWidgetItem *item,
    const loglib::LogConfiguration::AnchorEntry &entry,
    const LogModel *model,
    ThemeControl *theme,
    int swatchPx
)
{
    const QString displayPath = DisplayPathForLocator(model, entry.locator);
    const QString filename = FilenameFromDisplayPath(displayPath);
    const QString label = filename.isEmpty() ? AnchorsDock::tr("line %1").arg(entry.lineId)
                                             : AnchorsDock::tr("line %1 - %2").arg(entry.lineId).arg(filename);
    const QString noteText = QString::fromStdString(entry.note);

    item->setIcon(COLUMN_ANCHOR, SwatchIconFor(theme, entry.colorIndex, swatchPx));
    item->setText(COLUMN_ANCHOR, label);
    item->setData(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE, QString::fromStdString(entry.locator));
    item->setData(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE, QVariant::fromValue<qulonglong>(entry.lineId));

    // Item-wide editable flag (Qt has no per-column equivalent);
    // `NoEditDelegate` on `COLUMN_ANCHOR` enforces per-column
    // read-only behaviour.
    item->setFlags(item->flags() | Qt::ItemIsEditable);

    // `<qt>` wrapper forces rich-text mode; without it Qt's
    // `mightBeRichText` heuristic misfires on notes containing
    // `&` or `>` but no `<`.
    const QString tooltipHtml = BuildAnchorTooltipHtml(entry.colorIndex, entry.lineId, displayPath, noteText);
    item->setToolTip(COLUMN_ANCHOR, tooltipHtml);
    item->setToolTip(COLUMN_NOTE, tooltipHtml);
}

} // namespace

AnchorsDock::AnchorsDock(AnchorManager *anchors, LogModel *model, ThemeControl *theme, QWidget *parent)
    : QDockWidget(tr("Anchors"), parent), mAnchors(anchors), mModel(model), mTheme(theme)
{
    setObjectName(QStringLiteral("anchorsDock"));

    auto *host = new QWidget(this);
    auto *layout = new QVBoxLayout(host);
    layout->setContentsMargins(4, 4, 4, 4);
    layout->setSpacing(4);

    auto *header = new QHBoxLayout();
    mClearAllButton = new QPushButton(tr("Clear all"), host);
    mClearAllButton->setObjectName(QStringLiteral("anchorsClearAll"));
    // `Refresh()` enables this once there is something to clear.
    mClearAllButton->setEnabled(false);
    header->addStretch(1);
    header->addWidget(mClearAllButton);
    layout->addLayout(header);

    mTree = new QTreeWidget(host);
    mTree->setObjectName(QStringLiteral("anchorsList"));
    mTree->setColumnCount(2);
    mTree->setHeaderLabels({tr("Anchor"), tr("Note")});
    mTree->setRootIsDecorated(false);
    mTree->setUniformRowHeights(true);
    mTree->setSelectionMode(QAbstractItemView::ExtendedSelection);
    mTree->setContextMenuPolicy(Qt::CustomContextMenu);
    // Inline note editing: double-click on the note column or F2.
    // `NoEditDelegate` on `COLUMN_ANCHOR` (below) blocks editor
    // construction there, so F2 on the anchor column is a no-op
    // rather than opening an editor for the display label.
    mTree->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    // Parented to the tree so the delegate lives as long as the
    // column it guards.
    mTree->setItemDelegateForColumn(COLUMN_ANCHOR, new NoEditDelegate(mTree));
    // Note column carries most of the content; stretch so long
    // notes don't clip.
    if (auto *headerView = mTree->header(); headerView != nullptr)
    {
        headerView->setSectionResizeMode(COLUMN_ANCHOR, QHeaderView::ResizeToContents);
        headerView->setSectionResizeMode(COLUMN_NOTE, QHeaderView::Stretch);
        headerView->setStretchLastSection(false);
    }
    layout->addWidget(mTree, 1);

    setWidget(host);

    // All three wired paths gate on visibility so a buried dock
    // pays nothing. `anchorChanged` and `anchorNoteChanged` take
    // scoped per-key handlers so an unrelated mutation (Ctrl+1..8
    // elsewhere, streaming FIFO eviction, ...) doesn't clear the
    // tree and drop an in-flight inline note edit. Bulk resets
    // (`ClearAll`, `Replace`, multi-key ops) take the full-rebuild
    // path -- rare during active editing.
    if (mAnchors != nullptr)
    {
        connect(mAnchors, &AnchorManager::anchorChanged, this, &AnchorsDock::OnAnchorChanged);
        connect(mAnchors, &AnchorManager::anchorNoteChanged, this, &AnchorsDock::OnAnchorNoteChanged);
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

    // Two paths into the jump handler: `itemDoubleClicked` is the
    // primary mouse gesture (always fires on left double-click);
    // `itemActivated` covers keyboard Enter / Return and, on some
    // styles, double-click too. `OnItemActivated` filters note-
    // column activations so double-click there opens the inline
    // editor (via the tree's `DoubleClicked` edit trigger) instead
    // of jumping.
    connect(mTree, &QTreeWidget::itemDoubleClicked, this, &AnchorsDock::OnItemActivated);
    connect(mTree, &QTreeWidget::itemActivated, this, &AnchorsDock::OnItemActivated);
    connect(mTree, &QTreeWidget::itemChanged, this, &AnchorsDock::OnItemChanged);
    connect(mTree, &QWidget::customContextMenuRequested, this, &AnchorsDock::OnContextMenuRequested);
    connect(mClearAllButton, &QPushButton::clicked, this, &AnchorsDock::OnClearAllClicked);

    // Veto the window-scope `F2` shortcut when focus is on the
    // note column so the inline editor opens instead of the
    // "Jump to next anchor" action firing. See `eventFilter`.
    mTree->installEventFilter(this);

    // Bypass the visibility gate here: visibility just opened, so
    // this call is the first `RefreshAlways` a session sees. Don't
    // call `Refresh()` in the ctor -- the dock starts hidden.
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
    // survives the `clear()` + repopulate below. Selection lives in
    // an `unordered_set` so restore stays O(N) even at hundreds of
    // selected anchors. Focus column is captured too so a mid-edit
    // refresh doesn't teleport back to `COLUMN_ANCHOR`.
    AnchorKeyCarrier focusedKey;
    bool hadFocus = false;
    int focusedColumn = COLUMN_ANCHOR;
    if (const QTreeWidgetItem *focused = mTree->currentItem(); focused != nullptr)
    {
        focusedKey.locator = focused->data(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE).toString();
        focusedKey.lineId = focused->data(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE).toULongLong();
        hadFocus = true;
        if (const int currentColumn = mTree->currentColumn(); currentColumn == COLUMN_NOTE)
        {
            focusedColumn = COLUMN_NOTE;
        }
    }
    std::unordered_set<AnchorKeyCarrier, AnchorKeyCarrierHash> selectedKeys;
    const auto selectedItems = mTree->selectedItems();
    selectedKeys.reserve(static_cast<std::size_t>(selectedItems.size()));
    for (const QTreeWidgetItem *selected : selectedItems)
    {
        selectedKeys.insert(
            AnchorKeyCarrier{
                .locator = selected->data(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE).toString(),
                .lineId = selected->data(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE).toULongLong(),
            }
        );
    }

    // Suppress the `itemChanged` cascade that `clear()` + `setText`
    // would otherwise fire back into `SetAnchorNote`. Scope guard
    // keeps the counter balanced across an allocator throw.
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
        auto *item = new QTreeWidgetItem(mTree);
        PopulateAnchorCellForEntry(item, entry, mModel.data(), mTheme.data(), swatchPx);
        // Full rebuild owns the note cell: no in-flight editor can
        // exist here since we just cleared the tree.
        item->setText(COLUMN_NOTE, QString::fromStdString(entry.note));
    }

    if (mClearAllButton != nullptr)
    {
        mClearAllButton->setEnabled(!mAnchors->Empty());
    }

    // Restore selection + focus under a signal blocker so observers
    // see a single selectionChanged. Vanished items are dropped.
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
            const AnchorKeyCarrier itemKey{
                .locator = item->data(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE).toString(),
                .lineId = item->data(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE).toULongLong(),
            };
            if (selectedKeys.contains(itemKey))
            {
                item->setSelected(true);
            }
            if (hadFocus && itemKey == focusedKey)
            {
                mTree->setCurrentItem(item, focusedColumn, QItemSelectionModel::NoUpdate);
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

bool AnchorsDock::eventFilter(QObject *watched, QEvent *event)
{
    // Qt sends `ShortcutOverride` to the focus widget before matching
    // a window-scope shortcut. Accepting it here vetoes the shortcut
    // and makes Qt deliver a fresh `KeyPress` to the widget, which
    // the tree's `EditKeyPressed` trigger picks up to open the note
    // editor. Without this, `MainWindow`'s window-scope F2 ("Jump
    // to next anchor") would swallow the key.
    //
    // Only vetoed on plain F2 with `COLUMN_NOTE` focused: on the
    // anchor column there's no editor to open (`NoEditDelegate`
    // refuses), so we let the jump shortcut fire. `Shift+F2` is
    // left alone -- the tree ignores it, so the "jump previous"
    // shortcut still works from within the dock.
    if (watched == mTree && event->type() == QEvent::ShortcutOverride)
    {
        // NOLINT: `static_cast` after a `type()` gate is Qt's
        // standard downcast idiom for events.
        auto *keyEvent = static_cast<QKeyEvent *>(event); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        if (keyEvent->key() == Qt::Key_F2 && keyEvent->modifiers() == Qt::NoModifier &&
            mTree->currentColumn() == COLUMN_NOTE)
        {
            keyEvent->accept();
            return true;
        }
    }
    return QDockWidget::eventFilter(watched, event);
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
    // Only the anchor column jumps. On the note column Enter is
    // reserved for commit-edit and double-click opens the inline
    // editor -- rerouting either to a jump would swallow the
    // user's intent to type.
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
    // Only the note column is user-editable; drop mutations on
    // column 0 defensively.
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
        // Newline/tab typed: reflect the sanitised form back into
        // the item so the display matches what we're persisting.
        ++mSuppressItemChanged;
        const auto suppressGuard = qScopeGuard([this] { --mSuppressItemChanged; });
        item->setText(COLUMN_NOTE, sanitised);
    }
    // `SetAnchorNote` may trigger a full-rebuild refresh (fallback
    // path in `OnAnchorNoteChanged`); don't touch `item` past this
    // line without re-resolving it. `Entries()` filters runtime-
    // only anchors out of the tree, so no persistence warning is
    // needed here.
    mAnchors->SetAnchorNote(key, sanitised.toStdString());
}

void AnchorsDock::OnContextMenuRequested(const QPoint &pos)
{
    if (mTree == nullptr || mAnchors.isNull())
    {
        return;
    }
    const QTreeWidgetItem *item = mTree->itemAt(pos);
    if (item == nullptr)
    {
        return;
    }

    // Copy the key + selection out before `exec()` pumps events:
    // a queued anchor signal could rebuild the tree and invalidate
    // `item`. Source row is resolved *after* `exec()` since a
    // mid-popup eviction would shift indices.
    const AnchorManager::Key key{
        .locator = item->data(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE).toString().toStdString(),
        .lineId = item->data(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE).toULongLong(),
    };

    // Snapshot the whole selection so a multi-select bulk-remove
    // can operate on it after `exec()` returns. The right-clicked
    // item is always included (Qt doesn't auto-extend the selection
    // to it), matches what `ClearAnchorOnSelection` on the table
    // view does.
    std::vector<AnchorManager::Key> selectedKeys;
    {
        const auto selectedItems = mTree->selectedItems();
        selectedKeys.reserve(static_cast<std::size_t>(selectedItems.size()) + 1);
        selectedKeys.push_back(key);
        for (const QTreeWidgetItem *selected : selectedItems)
        {
            if (selected == nullptr || selected == item)
            {
                continue;
            }
            AnchorManager::Key selectedKey{
                .locator = selected->data(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE).toString().toStdString(),
                .lineId = selected->data(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE).toULongLong(),
            };
            selectedKeys.push_back(std::move(selectedKey));
        }
    }
    const bool multiRemove = selectedKeys.size() > 1;

    QMenu menu(this);
    const QAction *jumpAction = menu.addAction(tr("Jump to anchor"));
    // Edit note is always per-anchor (no "bulk edit note"); the
    // label refers to the right-clicked row regardless of selection.
    const QAction *editNoteAction = menu.addAction(tr("Edit note"));
    // Retitle for a multi-select bulk-remove so the fan-out is
    // visible up front.
    const QAction *removeAction = menu.addAction(
        multiRemove ? tr("Remove %1 anchors").arg(selectedKeys.size()) : tr("Remove anchor")
    );
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
        // Bulk path collapses to a single `anchorsReset` when >1 key
        // was removed, so downstream refresh stays cheap.
        mAnchors->RemoveAnchors(selectedKeys);
    }
}

void AnchorsDock::OnClearAllClicked()
{
    if (!mAnchors.isNull())
    {
        mAnchors->ClearAll();
    }
}

void AnchorsDock::OnAnchorChanged(const AnchorManager::Key &key)
{
    // Buried docks skip: a visibility flip drives a full refresh.
    if (!IsVisibleForRefresh() || mTree == nullptr || mAnchors.isNull())
    {
        return;
    }

    // Linear scan for the item -- the tree is at most a few hundred
    // rows, and a parallel lookup table would have to survive theme
    // flips / reorders.
    QTreeWidgetItem *existing = nullptr;
    const QString keyLocator = QString::fromStdString(key.locator);
    const auto keyLineId = static_cast<qulonglong>(key.lineId);
    for (int row = 0; row < mTree->topLevelItemCount(); ++row)
    {
        QTreeWidgetItem *candidate = mTree->topLevelItem(row);
        if (candidate == nullptr)
        {
            continue;
        }
        if (candidate->data(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE).toString() == keyLocator &&
            candidate->data(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE).toULongLong() == keyLineId)
        {
            existing = candidate;
            break;
        }
    }

    const auto colourOpt = mAnchors->ColorFor(key);

    // Removal branch: delete the item (if present) and leave every
    // other row alone -- an in-flight inline edit elsewhere must
    // survive an unrelated eviction.
    if (!colourOpt.has_value())
    {
        if (existing != nullptr)
        {
            ++mSuppressItemChanged;
            const auto suppressGuard = qScopeGuard([this] { --mSuppressItemChanged; });
            const int idx = mTree->indexOfTopLevelItem(existing);
            if (idx >= 0)
            {
                delete mTree->takeTopLevelItem(idx);
            }
        }
        if (mClearAllButton != nullptr)
        {
            mClearAllButton->setEnabled(!mAnchors->Empty());
        }
        return;
    }

    // Runtime-only anchors are filtered out of the tree by
    // `Entries()`; mirror the filter here. Still refresh the
    // "Clear all" enable state -- `Empty()` counts runtime-only
    // anchors, and the button drives `ClearAll` (which wipes them).
    if (key.locator.empty())
    {
        if (mClearAllButton != nullptr)
        {
            mClearAllButton->setEnabled(!mAnchors->Empty());
        }
        return;
    }

    // Note text stays out of this update: on recolour it hasn't
    // changed, on insert the new item starts empty (matching what
    // `SetAnchor` seeds); the note flow goes through
    // `OnAnchorNoteChanged` instead.
    const loglib::LogConfiguration::AnchorEntry entry{
        .locator = key.locator,
        .lineId = key.lineId,
        .colorIndex = *colourOpt,
        .note = mAnchors->NoteFor(key).value_or(std::string{}),
    };

    // Suppress `itemChanged` re-entry from `setText` / `setIcon`
    // so we don't feed the new label back into `SetAnchorNote`.
    ++mSuppressItemChanged;
    const auto suppressGuard = qScopeGuard([this] { --mSuppressItemChanged; });

    const int swatchPx = SwatchIconPixels(this);

    if (existing != nullptr)
    {
        PopulateAnchorCellForEntry(existing, entry, mModel.data(), mTheme.data(), swatchPx);
        if (mClearAllButton != nullptr)
        {
            mClearAllButton->setEnabled(!mAnchors->Empty());
        }
        return;
    }

    // Insert at the sorted position (`(locator, lineId)` ascending)
    // that `Entries()` would use so a later full rebuild produces
    // the same order. Comparison uses the canonical `std::string`
    // locator (matches `BuildSortedEntries`); UTF-16 code-unit and
    // UTF-8 byte orderings diverge on non-BMP characters.
    int insertPos = mTree->topLevelItemCount();
    for (int row = 0; row < mTree->topLevelItemCount(); ++row)
    {
        const QTreeWidgetItem *candidate = mTree->topLevelItem(row);
        if (candidate == nullptr)
        {
            continue;
        }
        const std::string candLocator =
            candidate->data(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE).toString().toStdString();
        const auto candLineId = candidate->data(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE).toULongLong();
        const bool candGreater = (candLocator > key.locator) ||
                                 (candLocator == key.locator && candLineId > static_cast<qulonglong>(key.lineId));
        if (candGreater)
        {
            insertPos = row;
            break;
        }
    }
    auto *newItem = new QTreeWidgetItem();
    PopulateAnchorCellForEntry(newItem, entry, mModel.data(), mTheme.data(), swatchPx);
    // Insert path owns the note cell too: no editor can exist on
    // an item that doesn't yet exist.
    newItem->setText(COLUMN_NOTE, QString::fromStdString(entry.note));
    mTree->insertTopLevelItem(insertPos, newItem);

    if (mClearAllButton != nullptr)
    {
        mClearAllButton->setEnabled(!mAnchors->Empty());
    }
}

void AnchorsDock::OnAnchorNoteChanged(const AnchorManager::Key &key)
{
    // Buried docks skip: a visibility flip drives a full refresh.
    if (!IsVisibleForRefresh() || mTree == nullptr || mAnchors.isNull())
    {
        return;
    }

    // Linear scan matches `OnAnchorChanged` rationale: the tree is
    // small and parallel lookup tables would have to survive
    // theme flips / reorders.
    QTreeWidgetItem *target = nullptr;
    const QString keyLocator = QString::fromStdString(key.locator);
    const auto keyLineId = static_cast<qulonglong>(key.lineId);
    for (int row = 0; row < mTree->topLevelItemCount(); ++row)
    {
        QTreeWidgetItem *candidate = mTree->topLevelItem(row);
        if (candidate == nullptr)
        {
            continue;
        }
        if (candidate->data(COLUMN_ANCHOR, ANCHOR_KEY_LOCATOR_ROLE).toString() == keyLocator &&
            candidate->data(COLUMN_ANCHOR, ANCHOR_KEY_LINE_ID_ROLE).toULongLong() == keyLineId)
        {
            target = candidate;
            break;
        }
    }
    if (target == nullptr)
    {
        // Key not currently rendered (dock was hidden when the
        // anchor was created, or a bulk reset raced the signal).
        RefreshAlways();
        return;
    }

    const auto note = mAnchors->NoteFor(key);
    if (!note.has_value())
    {
        // Anchor removed between `SetAnchorNote` and this slot
        // (queued-signal race). Full refresh drops the row.
        RefreshAlways();
        return;
    }
    const auto colorIndex = mAnchors->ColorFor(key).value_or(std::uint8_t{0});

    const QString noteText = QString::fromStdString(*note);
    const QString displayPath = DisplayPathForLocator(mModel.data(), key.locator);
    const QString tooltipHtml = BuildAnchorTooltipHtml(colorIndex, key.lineId, displayPath, noteText);

    // Suppress the `itemChanged` re-entry from `setText` so we
    // don't feed the text back into `SetAnchorNote`.
    ++mSuppressItemChanged;
    const auto suppressGuard = qScopeGuard([this] { --mSuppressItemChanged; });
    if (target->text(COLUMN_NOTE) != noteText)
    {
        target->setText(COLUMN_NOTE, noteText);
    }
    target->setToolTip(COLUMN_ANCHOR, tooltipHtml);
    target->setToolTip(COLUMN_NOTE, tooltipHtml);
}

void AnchorsDock::BeginEditingCurrentNote()
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
    // Force focus onto the tree so the editor actually grabs the
    // keyboard: F4 can reach here from "Clear all" or the tree
    // header, in which case some styles open the editor without
    // routing keystrokes to it.
    mTree->setFocus(Qt::ShortcutFocusReason);
    // Retarget to `COLUMN_NOTE`: `editItem` on `COLUMN_ANCHOR`
    // is a silent no-op (`NoEditDelegate` refuses to build an
    // editor), so mirror what a user's double-click / F2 does.
    mTree->setCurrentItem(item, COLUMN_NOTE);
    mTree->editItem(item, COLUMN_NOTE);
}
