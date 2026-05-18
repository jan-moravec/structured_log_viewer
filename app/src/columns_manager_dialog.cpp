#include "columns_manager_dialog.hpp"

#include "column_editor.hpp"
#include "log_model.hpp"
#include "main_window.hpp"

#include <loglib/log_configuration.hpp>

#include <QDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace
{
constexpr int DIALOG_INITIAL_WIDTH = 640;
constexpr int DIALOG_INITIAL_HEIGHT = 420;
constexpr int COL_HEADER = 0;
constexpr int COL_KEYS = 1;
constexpr int COL_TYPE = 2;
constexpr int COL_AUTODETECT = 3;
constexpr int COL_VISIBLE = 4;
constexpr int COL_COUNT = 5;

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
} // namespace

ColumnsManagerDialog::ColumnsManagerDialog(LogModel *model, MainWindow *mainWindow, QWidget *parent)
    : QDialog(parent), mModel(model), mMainWindow(mainWindow)
{
    setWindowTitle(tr("Manage Columns"));
    setObjectName(QStringLiteral("ColumnsManagerDialog"));
    resize(DIALOG_INITIAL_WIDTH, DIALOG_INITIAL_HEIGHT);

    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(
        tr("Reorder columns with Move up / Move down, toggle visibility in the "
           "Visible column, or use Edit\u2026 for the full per-column editor."),
        this
    );
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto *body = new QHBoxLayout();
    layout->addLayout(body, 1);

    mTable = new QTableWidget(this);
    mTable->setObjectName(QStringLiteral("columnsTable"));
    mTable->setColumnCount(COL_COUNT);
    const QStringList headers{
        tr("Header"), tr("Keys"), tr("Type"), tr("Auto-detect"), tr("Visible"),
    };
    mTable->setHorizontalHeaderLabels(headers);
    mTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    mTable->setSelectionMode(QAbstractItemView::SingleSelection);
    mTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mTable->verticalHeader()->setVisible(false);
    mTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    mTable->horizontalHeader()->setStretchLastSection(false);
    body->addWidget(mTable, 1);

    auto *buttons = new QVBoxLayout();
    mMoveUpButton = new QPushButton(tr("Move up"), this);
    mMoveUpButton->setObjectName(QStringLiteral("moveUpButton"));
    mMoveDownButton = new QPushButton(tr("Move down"), this);
    mMoveDownButton->setObjectName(QStringLiteral("moveDownButton"));
    mEditButton = new QPushButton(tr("Edit\u2026"), this);
    mEditButton->setObjectName(QStringLiteral("editButton"));
    buttons->addWidget(mMoveUpButton);
    buttons->addWidget(mMoveDownButton);
    buttons->addWidget(mEditButton);
    buttons->addStretch(1);
    body->addLayout(buttons);

    auto *closeRow = new QHBoxLayout();
    closeRow->addStretch(1);
    mCloseButton = new QPushButton(tr("Close"), this);
    mCloseButton->setObjectName(QStringLiteral("closeButton"));
    mCloseButton->setDefault(true);
    closeRow->addWidget(mCloseButton);
    layout->addLayout(closeRow);

    connect(mMoveUpButton, &QPushButton::clicked, this, &ColumnsManagerDialog::MoveSelectedUp);
    connect(mMoveDownButton, &QPushButton::clicked, this, &ColumnsManagerDialog::MoveSelectedDown);
    connect(mEditButton, &QPushButton::clicked, this, &ColumnsManagerDialog::EditSelected);
    connect(mCloseButton, &QPushButton::clicked, this, &QDialog::close);

    // Double-click any row to open the editor; mirrors the
    // diagnostics dialog drill-down so users have a single mental
    // model for "drill into this column".
    connect(mTable, &QTableWidget::cellDoubleClicked, this, [this](int, int) { EditSelected(); });

    connect(mTable, &QTableWidget::itemChanged, this, &ColumnsManagerDialog::OnItemChanged);

    if (mModel)
    {
        // Any out-of-band change to the column shape (header drag,
        // streaming-driven type promotion, configuration load) must
        // be reflected here so the manager never lies to the user.
        connect(mModel, &LogModel::modelReset, this, &ColumnsManagerDialog::Refresh);
        connect(
            mModel,
            &LogModel::headerDataChanged,
            this,
            [this](Qt::Orientation, int, int) { Refresh(); }
        );
        connect(mModel, &LogModel::columnHealthChanged, this, &ColumnsManagerDialog::Refresh);
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
    for (int i = 0; i < static_cast<int>(columns.size()); ++i)
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
    if (row < 0 || row >= static_cast<int>(columns.size()))
    {
        return;
    }
    const auto &column = columns[static_cast<size_t>(row)];

    mTable->setItem(row, COL_HEADER, MakeReadOnlyItem(QString::fromStdString(column.header)));
    mTable->setItem(row, COL_KEYS, MakeReadOnlyItem(FormatKeys(column.keys)));
    mTable->setItem(row, COL_TYPE, MakeReadOnlyItem(FormatType(column.type, column.autoDetect)));
    mTable->setItem(row, COL_AUTODETECT, MakeReadOnlyItem(column.autoDetect ? tr("Yes") : tr("No")));

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
    if (row < 0 || row >= static_cast<int>(columns.size()))
    {
        return;
    }
    const bool visible = item->checkState() == Qt::Checked;
    if (mMainWindow)
    {
        // Going through `MainWindow::SetColumnVisible` keeps the
        // header `setSectionHidden` flag, the View menu's checked
        // state, and the sort-on-hidden-column reset in lockstep
        // with the lib mutation. Calling the lib mutator directly
        // would skip those side-effects.
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
        // visibility / status-bar refresh fires identically to the
        // header context-menu path. The dialog itself observes the
        // resulting `headerDataChanged` and refreshes its rows
        // through the connected lambda.
        mMainWindow->EditColumn(row);
    }
    else
    {
        ColumnEditor editor(mModel, row, this);
        editor.exec();
        Refresh();
    }
}
