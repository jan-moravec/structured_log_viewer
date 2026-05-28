#include "columns_manager_dialog.hpp"

#include "column_editor.hpp"
#include "log_model.hpp"
#include "main_window.hpp"

#include <loglib/log_configuration.hpp>

#include <QApplication>
#include <QDialog>
#include <QFont>
#include <QFontMetrics>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <utility>

namespace
{
constexpr int DIALOG_INITIAL_WIDTH = 720;
constexpr int DIALOG_INITIAL_HEIGHT = 440;
constexpr int COL_HEADER = 0;
constexpr int COL_KEYS = 1;
constexpr int COL_TYPE = 2;
constexpr int COL_AUTODETECT = 3;
constexpr int COL_VISIBLE = 4;
constexpr int COL_COUNT = 5;
// Visual spacing tokens; matches Qt's stock dialog dressing.
constexpr int OUTER_MARGIN = 16;
constexpr int SECTION_SPACING = 12;
constexpr int BUTTON_SPACING = 8;
constexpr int BUTTON_MIN_WIDTH = 96;
constexpr int ROW_VERTICAL_PADDING = 8;

QString FormatType(loglib::LogConfiguration::Type type, bool autoDetect)
{
    using Type = loglib::LogConfiguration::Type;
    QString base;
    switch (type)
    {
    case Type::Any:
        base = QStringLiteral("Any");
        break;
    case Type::String:
        base = QStringLiteral("String");
        break;
    case Type::Boolean:
        base = QStringLiteral("Boolean");
        break;
    case Type::Integer:
        base = QStringLiteral("Integer");
        break;
    case Type::Floating:
        base = QStringLiteral("Floating-point");
        break;
    case Type::Number:
        base = QStringLiteral("Number");
        break;
    case Type::Time:
        base = QStringLiteral("Time");
        break;
    case Type::Enumeration:
        base = QStringLiteral("Enumeration");
        break;
    case Type::Level:
        base = QStringLiteral("Level");
        break;
    }
    if (autoDetect && type == Type::Any)
    {
        return QStringLiteral("Auto-detect");
    }
    return base;
}

QString FormatKeys(const std::vector<std::string> &keys)
{
    QStringList out;
    out.reserve(static_cast<int>(keys.size()));
    for (const auto &k : keys)
    {
        out.append(QString::fromStdString(k));
    }
    return out.join(QStringLiteral(", "));
}

QTableWidgetItem *MakeReadOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QTableWidgetItem *MakeReadOnlyCenteredItem(const QString &text)
{
    auto *item = MakeReadOnlyItem(text);
    item->setTextAlignment(Qt::AlignCenter);
    return item;
}
} // namespace

ColumnsManagerDialog::ColumnsManagerDialog(LogModel *model, MainWindow *mainWindow, QWidget *parent)
    : QDialog(parent), mModel(model), mMainWindow(mainWindow)
{
    setWindowTitle(tr("Manage Columns"));
    setObjectName(QStringLiteral("ColumnsManagerDialog"));
    resize(DIALOG_INITIAL_WIDTH, DIALOG_INITIAL_HEIGHT);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(OUTER_MARGIN, OUTER_MARGIN, OUTER_MARGIN, OUTER_MARGIN);
    layout->setSpacing(SECTION_SPACING);

    mIntroLabel = new QLabel(
        tr("Reorder columns with Move up / Move down, toggle visibility in the "
           "Visible column, or use Edit\u2026 for the full per-column editor."),
        this
    );
    mIntroLabel->setObjectName(QStringLiteral("introLabel"));
    mIntroLabel->setWordWrap(true);
    // Use the palette's PlaceholderText role so the helper text picks
    // up the muted tone of the active theme (light or dark).
    // `RefreshPalette` re-applies this when the active theme flips.
    RefreshPalette();
    layout->addWidget(mIntroLabel);

    auto *body = new QHBoxLayout();
    body->setSpacing(SECTION_SPACING);
    layout->addLayout(body, 1);

    mTable = new QTableWidget(this);
    mTable->setObjectName(QStringLiteral("columnsTable"));
    mTable->setColumnCount(COL_COUNT);
    const QStringList headers{
        tr("Header"),
        tr("Keys"),
        tr("Type"),
        tr("Auto-detect"),
        tr("Visible"),
    };
    mTable->setHorizontalHeaderLabels(headers);
    mTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    mTable->setSelectionMode(QAbstractItemView::SingleSelection);
    mTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mTable->setAlternatingRowColors(true);
    mTable->setShowGrid(false);
    mTable->verticalHeader()->setVisible(false);

    // Bold + left-aligned header labels so they don't blend with
    // the data rows on dark themes.
    QHeaderView *hHeader = mTable->horizontalHeader();
    QFont headerFont = hHeader->font();
    headerFont.setBold(true);
    hHeader->setFont(headerFont);
    hHeader->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    hHeader->setHighlightSections(false);

    // Stretch the wide `Keys` column to fill leftover width; pin
    // the categorical columns to their content.
    hHeader->setSectionResizeMode(COL_HEADER, QHeaderView::Interactive);
    hHeader->setSectionResizeMode(COL_KEYS, QHeaderView::Stretch);
    hHeader->setSectionResizeMode(COL_TYPE, QHeaderView::ResizeToContents);
    hHeader->setSectionResizeMode(COL_AUTODETECT, QHeaderView::ResizeToContents);
    hHeader->setSectionResizeMode(COL_VISIBLE, QHeaderView::ResizeToContents);
    hHeader->setStretchLastSection(false);

    // Row height derived from font metrics so DPI / font-size
    // changes scale with it.
    const int derivedRowHeight = mTable->fontMetrics().height() + (ROW_VERTICAL_PADDING * 2);
    mTable->verticalHeader()->setDefaultSectionSize(derivedRowHeight);

    body->addWidget(mTable, 1);

    auto *buttons = new QVBoxLayout();
    buttons->setSpacing(BUTTON_SPACING);
    mMoveUpButton = new QPushButton(tr("Move up"), this);
    mMoveUpButton->setObjectName(QStringLiteral("moveUpButton"));
    mMoveDownButton = new QPushButton(tr("Move down"), this);
    mMoveDownButton->setObjectName(QStringLiteral("moveDownButton"));
    mEditButton = new QPushButton(tr("Edit\u2026"), this);
    mEditButton->setObjectName(QStringLiteral("editButton"));
    // Uniform minimum width so the right-rail buttons line up.
    for (QPushButton *button : {mMoveUpButton, mMoveDownButton, mEditButton})
    {
        button->setMinimumWidth(BUTTON_MIN_WIDTH);
    }
    buttons->addWidget(mMoveUpButton);
    buttons->addWidget(mMoveDownButton);
    buttons->addWidget(mEditButton);
    buttons->addStretch(1);
    body->addLayout(buttons);

    // Separator above the footer so the lone Close button is
    // visually anchored rather than floating.
    auto *separator = new QFrame(this);
    separator->setObjectName(QStringLiteral("footerSeparator"));
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    layout->addWidget(separator);

    auto *closeRow = new QHBoxLayout();
    closeRow->addStretch(1);
    mCloseButton = new QPushButton(tr("Close"), this);
    mCloseButton->setObjectName(QStringLiteral("closeButton"));
    mCloseButton->setDefault(true);
    mCloseButton->setMinimumWidth(BUTTON_MIN_WIDTH);
    closeRow->addWidget(mCloseButton);
    layout->addLayout(closeRow);

    connect(mMoveUpButton, &QPushButton::clicked, this, &ColumnsManagerDialog::MoveSelectedUp);
    connect(mMoveDownButton, &QPushButton::clicked, this, &ColumnsManagerDialog::MoveSelectedDown);
    connect(mEditButton, &QPushButton::clicked, this, &ColumnsManagerDialog::EditSelected);
    connect(mCloseButton, &QPushButton::clicked, this, &QDialog::close);

    // Double-click a row to open the editor (mirrors the
    // diagnostics dialog drill-down).
    connect(mTable, &QTableWidget::cellDoubleClicked, this, [this](int, int) { EditSelected(); });

    connect(mTable, &QTableWidget::itemChanged, this, &ColumnsManagerDialog::OnItemChanged);

    if (mModel)
    {
        connect(mModel, &LogModel::modelReset, this, &ColumnsManagerDialog::Refresh);
        // Per-column re-render so a single column edit doesn't
        // rebuild every row.
        connect(mModel, &LogModel::headerDataChanged, this, [this](Qt::Orientation orientation, int first, int last) {
            if (orientation != Qt::Horizontal)
            {
                return;
            }
            RefreshRange(first, last);
        });
        // Reorders only fire `columnsMoved`; without this hook the
        // manager would keep showing the pre-move order.
        connect(mModel, &QAbstractItemModel::columnsMoved, this, [this]() { Refresh(); });
        // Skip rebuilds while hidden so frequent promote/demote
        // events during streaming don't churn an invisible widget.
        connect(mModel, &LogModel::columnHealthChanged, this, [this]() {
            if (isVisible())
            {
                Refresh();
            }
        });
    }

    Refresh();
}

void ColumnsManagerDialog::Refresh()
{
    if (!mModel)
    {
        return;
    }
    const auto &columns = mModel->Configuration().columns;
    mUpdatingProgrammatically = true;
    mTable->clearContents();
    mTable->setRowCount(static_cast<int>(columns.size()));
    for (int i = 0; std::cmp_less(i, columns.size()); ++i)
    {
        RebuildRow(i);
    }
    mUpdatingProgrammatically = false;
}

void ColumnsManagerDialog::RefreshRange(int firstColumn, int lastColumn)
{
    if (!mModel)
    {
        return;
    }
    const auto &columns = mModel->Configuration().columns;
    const int rowCount = static_cast<int>(columns.size());
    if (mTable->rowCount() != rowCount)
    {
        // Shape changed (column added / removed) -- the range
        // signal alone can't grow / shrink the layout.
        Refresh();
        return;
    }
    const int first = std::max(0, firstColumn);
    const int last = std::min(rowCount - 1, lastColumn);
    if (first > last)
    {
        return;
    }
    mUpdatingProgrammatically = true;
    for (int i = first; i <= last; ++i)
    {
        RebuildRow(i);
    }
    mUpdatingProgrammatically = false;
}

void ColumnsManagerDialog::RebuildRow(int row)
{
    if (!mModel)
    {
        return;
    }
    const auto &columns = mModel->Configuration().columns;
    if (row < 0 || std::cmp_greater_equal(row, columns.size()))
    {
        return;
    }
    const auto &column = columns[static_cast<size_t>(row)];

    mTable->setItem(row, COL_HEADER, MakeReadOnlyItem(QString::fromStdString(column.header)));
    mTable->setItem(row, COL_KEYS, MakeReadOnlyItem(FormatKeys(column.keys)));
    mTable->setItem(row, COL_TYPE, MakeReadOnlyItem(FormatType(column.type, column.autoDetect)));
    mTable->setItem(row, COL_AUTODETECT, MakeReadOnlyCenteredItem(column.autoDetect ? tr("Yes") : tr("No")));

    auto *visibleItem = new QTableWidgetItem();
    visibleItem->setFlags((visibleItem->flags() & ~Qt::ItemIsEditable) | Qt::ItemIsUserCheckable);
    visibleItem->setCheckState(column.visible ? Qt::Checked : Qt::Unchecked);
    visibleItem->setTextAlignment(Qt::AlignCenter);
    mTable->setItem(row, COL_VISIBLE, visibleItem);
}

void ColumnsManagerDialog::OnItemChanged(QTableWidgetItem *item)
{
    if (mUpdatingProgrammatically || !mModel || item == nullptr || item->column() != COL_VISIBLE)
    {
        return;
    }
    const int row = item->row();
    const auto &columns = mModel->Configuration().columns;
    if (row < 0 || std::cmp_greater_equal(row, columns.size()))
    {
        return;
    }
    const bool visible = item->checkState() == Qt::Checked;
    if (mMainWindow)
    {
        // Route through `MainWindow::SetColumnVisible` so the header
        // hide-flag, View-menu state, and sort-on-hidden-column
        // reset stay coherent with the lib mutation.
        mMainWindow->SetColumnVisible(row, visible);
    }
    else
    {
        mModel->ConfigurationManager().SetColumnVisible(static_cast<size_t>(row), visible);
    }
}

int ColumnsManagerDialog::CurrentRow() const
{
    if (mTable == nullptr)
    {
        return -1;
    }
    auto *const selection = mTable->selectionModel();
    if (selection == nullptr || !selection->hasSelection())
    {
        return -1;
    }
    const auto rows = selection->selectedRows();
    if (rows.isEmpty())
    {
        return -1;
    }
    return rows.first().row();
}

void ColumnsManagerDialog::MoveSelectedUp()
{
    if (!mModel)
    {
        return;
    }
    const int row = CurrentRow();
    if (row <= 0)
    {
        return;
    }
    if (mModel->MoveColumn(row, row - 1))
    {
        Refresh();
        mTable->selectRow(row - 1);
    }
}

void ColumnsManagerDialog::MoveSelectedDown()
{
    if (!mModel)
    {
        return;
    }
    const int row = CurrentRow();
    const int total = static_cast<int>(mModel->Configuration().columns.size());
    if (row < 0 || row >= total - 1)
    {
        return;
    }
    if (mModel->MoveColumn(row, row + 1))
    {
        Refresh();
        mTable->selectRow(row + 1);
    }
}

void ColumnsManagerDialog::RefreshPalette()
{
    if (mIntroLabel == nullptr)
    {
        return;
    }
    // Sample `PlaceholderText` from the app palette rather than the
    // label's own (overridden) palette -- once we've stamped a
    // foreground-role override here, reading back through
    // `mIntroLabel->palette()` can return the cached override for
    // roles the resolve-mask flags as locally-set. The app
    // palette is the source of truth for the theme-supplied value.
    QPalette introPalette = mIntroLabel->palette();
    introPalette.setColor(mIntroLabel->foregroundRole(), qApp->palette().color(QPalette::PlaceholderText));
    mIntroLabel->setPalette(introPalette);
}

void ColumnsManagerDialog::EditSelected()
{
    if (!mModel)
    {
        return;
    }
    const int row = CurrentRow();
    if (row < 0)
    {
        return;
    }
    if (mMainWindow)
    {
        // Route through `MainWindow::EditColumn` so the post-accept
        // visibility + status-bar refresh matches the header
        // context-menu path. Our own rows refresh via the connected
        // `headerDataChanged` lambda.
        mMainWindow->EditColumn(row);
    }
    else
    {
        ColumnEditor editor(mModel, row, this);
        editor.exec();
        Refresh();
    }
}
