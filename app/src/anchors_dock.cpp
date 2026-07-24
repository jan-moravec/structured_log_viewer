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

/// Snapshot of one anchor entry's identity as pulled off a
/// `QTreeWidgetItem`'s user-role data. Used by `RefreshAlways` to
/// remember selection + focus across a `clear()` + repopulate.
///
/// The type lives at file scope (not inside `RefreshAlways`) so it
/// can be used as a key in `std::unordered_set` cleanly: MSVC's
/// hash-set traits instantiate `_Nothrow_compare` on the key type,
/// and locally-scoped class types trigger a known ODR-ish
/// diagnostic path there.
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

/// Extract the filename portion of @p displayPath (a fully qualified
/// path in the user's display case). `QFileInfo` stays entirely inside
/// Qt's UTF-16 model, so non-ASCII paths (`логи/приложение.jsonl`)
/// survive round-trips that `std::filesystem::path(std::string)`
/// would mangle on Windows via the ANSI codepage.
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

/// Adapt `AnchorManager::SanitiseNote` (which is `std::string`-based
/// so it can be called from non-Qt code paths) to the `QString`
/// currency of the widget layer. Round-trip through UTF-8 keeps
/// non-ASCII notes intact.
[[nodiscard]] QString SanitiseNote(const QString &raw)
{
    return QString::fromStdString(AnchorManager::SanitiseNote(raw.toStdString()));
}

/// Build the `<qt>...</qt>`-wrapped tooltip HTML shared by both the
/// full `RefreshAlways` rebuild and the surgical
/// `OnAnchorNoteChanged` path. Kept in one place so the escaping /
/// wrapper rules (see the `<qt>`-envelope tests) can't drift.
[[nodiscard]] QString BuildAnchorTooltipHtml(
    std::uint8_t colorIndex, std::uint64_t lineId, const QString &displayPath, const QString &noteText
)
{
    const QString escapedDisplayPath = displayPath.toHtmlEscaped();
    const QString escapedNote = noteText.toHtmlEscaped();
    // Scope translations to `AnchorsDock` so Linguist groups the
    // dock's user-visible strings together instead of scattering
    // them under the untargetable `QObject` context.
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
    // short-circuits cheaply when the dock is buried. Note edits
    // fire the distinct `anchorNoteChanged` signal so the note
    // column reflects live edits from F4 / the row menu even
    // though colour didn't change.
    if (mAnchors != nullptr)
    {
        connect(mAnchors, &AnchorManager::anchorChanged, this, [this](const AnchorManager::Key &) { Refresh(); });
        // `anchorNoteChanged` gets its own scoped handler so a note
        // keystroke commit rewrites just the affected row rather
        // than rebuilding the whole tree (`RefreshAlways` is O(N)
        // per commit -- fine at ten anchors, less fine at hundreds).
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

    // Two paths into the "jump to anchor" handler:
    //   - `itemDoubleClicked` always fires on a left-button double
    //     click, regardless of the platform's
    //     `SH_ItemView_ActivateItemOnSingleClick` style hint. This is
    //     the primary mouse gesture -- double-click the anchor
    //     column to jump. The note column double-click opens the
    //     inline editor via the tree's `DoubleClicked` edit trigger;
    //     `OnItemActivated` filters double-clicks on the note column
    //     out so a keyboard-repeat double-emit is a no-op there.
    //   - `itemActivated` covers keyboard activation (Enter / Return
    //     on the selected item) and, on some Qt styles, also fires
    //     on double-click. `SelectSourceRow` is idempotent so a
    //     duplicate emit from a style that routes both signals is a
    //     harmless second navigate to the same row.
    connect(mTree, &QTreeWidget::itemDoubleClicked, this, &AnchorsDock::OnItemActivated);
    connect(mTree, &QTreeWidget::itemActivated, this, &AnchorsDock::OnItemActivated);
    connect(mTree, &QTreeWidget::itemChanged, this, &AnchorsDock::OnItemChanged);
    connect(mTree, &QWidget::customContextMenuRequested, this, &AnchorsDock::OnContextMenuRequested);
    connect(mClearAllButton, &QPushButton::clicked, this, &AnchorsDock::OnClearAllClicked);

    // Steal `F2` (and `Shift+F2`) from the window-scope "Jump to
    // (next|prev) anchor" `QAction` shortcuts when focus lives in
    // this tree so the dock's inline note editor actually opens.
    // See `eventFilter` for the mechanics.
    mTree->installEventFilter(this);

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
    //
    // `selectedKeys` goes into a hash set keyed on
    // `(locator, lineId)` so the restore loop stays O(N) even with
    // hundreds of anchors selected (the previous linear scan was
    // O(N * M) and started to bite during large-incident triage).
    // `AnchorKeyCarrier` is defined at file scope above -- MSVC's
    // unordered_set traits choke on a locally-scoped key type.
    AnchorKeyCarrier focusedKey;
    bool hadFocus = false;
    // Capture the current column too so an edit-in-flight on the
    // note column doesn't teleport back to the anchor column after
    // each refresh (jarring during a tabbed multi-row edit).
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
        const QString filename = FilenameFromDisplayPath(displayPath);
        const QString label = filename.isEmpty() ? tr("line %1").arg(entry.lineId)
                                                 : tr("line %1 - %2").arg(entry.lineId).arg(filename);
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

        // Tooltip rendering contract: wrap in `<qt>` to force
        // Qt's rich-text mode. Without the explicit wrapper Qt's
        // `mightBeRichText` heuristic misfires on paths / notes that
        // contain `&` or `>` but no `<`, and the escaped entities
        // (`&amp;`, `&gt;`) would render literally. Every
        // user-controlled substring is `toHtmlEscaped`; line breaks
        // are spelled `<br/>` because HTML collapses literal `\n`
        // to whitespace. Shared with `OnAnchorNoteChanged`'s surgical
        // path via `BuildAnchorTooltipHtml`.
        const QString tooltipHtml = BuildAnchorTooltipHtml(entry.colorIndex, entry.lineId, displayPath, noteText);
        item->setToolTip(COLUMN_ANCHOR, tooltipHtml);
        item->setToolTip(COLUMN_NOTE, tooltipHtml);
    }

    if (mClearAllButton != nullptr)
    {
        mClearAllButton->setEnabled(!mAnchors->Empty());
    }

    // Restore selection + focus in one pass so observers see a
    // single selection-changed signal. Vanished items are dropped.
    // Hash-set lookup keeps the loop O(N) irrespective of selection
    // size.
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
    // Qt dispatches a `ShortcutOverride` to the focused widget before
    // matching a shortcut. `event->accept()` on that dispatch tells
    // Qt "handle this as a normal key event, not a shortcut" -- which
    // makes the follow-up `KeyPress` land in `QAbstractItemView`'s
    // key handler, where `F2` triggers `EditKeyPressed`. Without this
    // filter, `MainWindow`'s window-scope `Qt::Key_F2` action ("Jump
    // to next anchor") would consume the event first and the inline
    // editor advertised in the tree's edit triggers would never open.
    //
    // Only shadow plain `F2`: every other shortcut still fires from
    // within the dock. In particular `Shift+F2` ("Jump to previous
    // anchor") is left alone so the user can jump backwards while
    // focus lives in the tree; the tree's key handler doesn't do
    // anything with `Shift+F2` on its own (`EditKeyPressed` matches
    // plain `F2` only), so nothing is stolen.
    if (watched == mTree && event->type() == QEvent::ShortcutOverride)
    {
        // `static_cast` is the standard Qt idiom after a `type()`
        // gate on a `QEvent` -- `dynamic_cast` requires RTTI on
        // every polymorphic event subclass and isn't part of Qt's
        // event-dispatch contract. NOLINT silences the generic
        // downcast lint without weakening the surrounding gate.
        auto *keyEvent = static_cast<QKeyEvent *>(event); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
        if (keyEvent->key() == Qt::Key_F2 && keyEvent->modifiers() == Qt::NoModifier)
        {
            keyEvent->accept();
            // Fall through so Qt still delivers the follow-up
            // `KeyPress` to the tree; we've only vetoed the shortcut
            // interpretation.
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
    // Wired to both `itemDoubleClicked` (primary mouse gesture) and
    // `itemActivated` (keyboard Enter, plus some styles' double-
    // click). Only the anchor column jumps: activating the note
    // column via Enter is reserved for "commit edit" and a double-
    // click on the note column opens the inline editor via the
    // tree's `DoubleClicked` edit trigger -- rerouting that gesture
    // to a jump would silently swallow the user's intent to type.
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
    // `AnchorManager::SetAnchorNote` emits `anchorNoteChanged`; the
    // dock refreshes via that signal, which restores selection +
    // focus around the edited item.
    //
    // Item-lifetime invariant: `SetAnchorNote` is a direct-connected
    // signal on the GUI thread that walks through `Refresh()` (which
    // does a `mTree->clear()` when it takes the full rebuild path).
    // The surgical note-refresh path we prefer keeps the item alive,
    // but a fallback full rebuild would invalidate `item`. Do NOT
    // touch `item` past this line -- copy anything you still need
    // out first (we don't).
    mAnchors->SetAnchorNote(key, sanitised.toStdString());
    // No runtime-only-anchor warning here: `Entries()` filters those
    // out of the tree, so `key.locator` is guaranteed non-empty on
    // any row the user could have double-clicked or F2'd into.
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

    // Snapshot the full selection now so a multi-select bulk-remove
    // can operate on it after `exec()` returns. The tree uses
    // `ExtendedSelection`, so silently defaulting to just the
    // right-clicked item when the user has ten anchors selected
    // reads like a bug from the user's side (`ClearAnchorOnSelection`
    // on the table view already treats a multi-row selection as the
    // target set; we mirror that here).
    //
    // The right-clicked item is *always* part of the effective set,
    // even when it isn't currently in the tree's selection: Qt
    // doesn't auto-extend the selection to the right-clicked row
    // for context menus. Insert it up front and de-dupe by key.
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
    // Edit note: the F2 shortcut lives on the tree's edit triggers,
    // not on the menu item -- popup shortcuts wouldn't do anything
    // useful since the menu is already the focus target. Editing is
    // per-anchor (there's no meaningful "bulk edit note"); the label
    // always refers to the right-clicked row regardless of selection.
    const QAction *editNoteAction = menu.addAction(tr("Edit note"));
    // Retitle "Remove anchor" -> "Remove N anchors" when the click
    // targets a multi-selection so the user can see up front that
    // the action will fan out.
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
        // Bulk path collapses to a single `anchorsReset` signal when
        // >1 key was removed, so downstream refresh stays cheap.
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

void AnchorsDock::OnAnchorNoteChanged(const AnchorManager::Key &key)
{
    // Buried docks skip the update; a visibility flip drives a full
    // `RefreshAlways`, so we don't need to catch up here.
    if (!IsVisibleForRefresh() || mTree == nullptr || mAnchors.isNull())
    {
        return;
    }

    // Locate the matching top-level item. The dock's row set is
    // one-item-per-anchor and typically small (dozens to a few
    // hundred); a linear scan is cheaper than maintaining a parallel
    // lookup table that has to survive theme flips / reorders.
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
        // Key isn't currently rendered (e.g. dock was hidden when
        // the anchor was created, or a bulk reset landed between
        // signal enqueue and delivery). Fall back to the full
        // rebuild path so the display catches up.
        RefreshAlways();
        return;
    }

    const auto note = mAnchors->NoteFor(key);
    if (!note.has_value())
    {
        // Anchor was removed between `SetAnchorNote` and this slot
        // (queued-signal race). A full refresh drops the row.
        RefreshAlways();
        return;
    }
    const auto colorIndex = mAnchors->ColorFor(key).value_or(std::uint8_t{0});

    const QString noteText = QString::fromStdString(*note);
    const QString displayPath = DisplayPathForLocator(mModel.data(), key.locator);
    const QString tooltipHtml = BuildAnchorTooltipHtml(colorIndex, key.lineId, displayPath, noteText);

    // Suppress the re-entrant `itemChanged` fired by `setText` so
    // we don't recursively feed the new text back into
    // `SetAnchorNote`. Scope guard keeps the counter balanced if
    // Qt's paint stack throws mid-write.
    ++mSuppressItemChanged;
    const auto suppressGuard = qScopeGuard([this] { --mSuppressItemChanged; });
    if (target->text(COLUMN_NOTE) != noteText)
    {
        target->setText(COLUMN_NOTE, noteText);
    }
    target->setToolTip(COLUMN_ANCHOR, tooltipHtml);
    target->setToolTip(COLUMN_NOTE, tooltipHtml);
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
