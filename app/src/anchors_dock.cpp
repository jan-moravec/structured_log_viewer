#include "anchors_dock.hpp"

#include "log_model.hpp"
#include "theme_control.hpp"

#include <QAction>
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
#include <QVBoxLayout>
#include <QVariant>
#include <QWidget>

#include <filesystem>
#include <string>

namespace
{

constexpr int SWATCH_ICON_PX = 14;
constexpr qreal SWATCH_PAINT_INSET = 0.5;
constexpr qreal SWATCH_CORNER_RADIUS = 3.0;

/// User-role payload carried by every `QListWidgetItem`: the
/// `(locator, lineId)` key that uniquely identifies the anchored
/// row in the live `LogModel`. Stored as a `QVariantMap` for
/// QVariant round-tripping.
constexpr int ANCHOR_KEY_LOCATOR_ROLE = Qt::UserRole + 1;
constexpr int ANCHOR_KEY_LINE_ID_ROLE = Qt::UserRole + 2;

[[nodiscard]] QIcon SwatchIconFor(ThemeControl *theme, std::uint8_t colorIndex)
{
    if (theme == nullptr)
    {
        return QIcon{};
    }
    const QBrush bg = theme->AnchorBrushFor(colorIndex, Qt::BackgroundRole);
    const QBrush fg = theme->AnchorBrushFor(colorIndex, Qt::ForegroundRole);
    QPixmap pix(SWATCH_ICON_PX, SWATCH_ICON_PX);
    pix.fill(Qt::transparent);
    QPainter painter(&pix);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(bg);
    painter.setPen(QPen(fg.color(), 1));
    painter.drawRoundedRect(
        QRectF(SWATCH_PAINT_INSET, SWATCH_PAINT_INSET, SWATCH_ICON_PX - 1, SWATCH_ICON_PX - 1),
        SWATCH_CORNER_RADIUS,
        SWATCH_CORNER_RADIUS
    );
    return QIcon(pix);
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

    if (mAnchors != nullptr)
    {
        connect(mAnchors, &AnchorManager::anchorChanged, this, [this](const AnchorManager::Key &) {
            if (IsVisibleForRefresh())
            {
                Refresh();
            }
        });
        connect(mAnchors, &AnchorManager::anchorsReset, this, [this]() {
            if (IsVisibleForRefresh())
            {
                Refresh();
            }
        });
    }

    // Repopulate when the underlying model's row set changes -- the
    // resolved "filename" column may flip on a streamed batch that
    // promotes a previously-empty locator. Cheap because Refresh
    // bails on an empty manager.
    if (mModel != nullptr)
    {
        connect(mModel, &QAbstractItemModel::modelReset, this, &AnchorsDock::Refresh);
    }

    connect(mList, &QListWidget::itemActivated, this, &AnchorsDock::OnItemActivated);
    connect(mList, &QListWidget::itemDoubleClicked, this, &AnchorsDock::OnItemActivated);
    connect(mList, &QWidget::customContextMenuRequested, this, &AnchorsDock::OnContextMenuRequested);
    connect(mClearAllButton, &QPushButton::clicked, this, &AnchorsDock::OnClearAllClicked);

    connect(this, &QDockWidget::visibilityChanged, this, [this](bool visible) {
        mPerceivedVisible = visible;
        if (visible)
        {
            Refresh();
        }
    });

    Refresh();
}

void AnchorsDock::Refresh()
{
    if (mList == nullptr)
    {
        return;
    }
    mList->clear();
    if (mAnchors == nullptr)
    {
        return;
    }

    const auto entries = mAnchors->Entries();
    for (const auto &entry : entries)
    {
        const QString filename = FilenameFromLocator(entry.locator);
        const QString label = filename.isEmpty()
                                  ? QObject::tr("line %1").arg(entry.lineId)
                                  : QObject::tr("line %1 - %2").arg(entry.lineId).arg(filename);

        auto *item = new QListWidgetItem(SwatchIconFor(mTheme.data(), entry.colorIndex), label, mList);
        item->setData(ANCHOR_KEY_LOCATOR_ROLE, QString::fromStdString(entry.locator));
        item->setData(ANCHOR_KEY_LINE_ID_ROLE, QVariant::fromValue<qulonglong>(entry.lineId));
        item->setToolTip(
            QString::fromStdString(entry.locator).isEmpty()
                ? QObject::tr("Anchor #%1, line %2").arg(entry.colorIndex + 1).arg(entry.lineId)
                : QObject::tr("Anchor #%1, line %2\n%3")
                      .arg(entry.colorIndex + 1)
                      .arg(entry.lineId)
                      .arg(QString::fromStdString(entry.locator))
        );
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
    AnchorManager::Key key{
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
    QListWidgetItem *item = mList->itemAt(pos);
    if (item == nullptr)
    {
        return;
    }

    QMenu menu(this);
    QAction *jumpAction = menu.addAction(QObject::tr("Jump to anchor"));
    QAction *removeAction = menu.addAction(QObject::tr("Remove anchor"));
    QAction *picked = menu.exec(mList->viewport()->mapToGlobal(pos));
    if (picked == nullptr)
    {
        return;
    }
    if (picked == jumpAction)
    {
        emit jumpToAnchorRequested(SourceRowForItem(item));
        return;
    }
    if (picked == removeAction)
    {
        AnchorManager::Key key{
            .locator = item->data(ANCHOR_KEY_LOCATOR_ROLE).toString().toStdString(),
            .lineId = item->data(ANCHOR_KEY_LINE_ID_ROLE).toULongLong(),
        };
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
