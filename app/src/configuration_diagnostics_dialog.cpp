#include "configuration_diagnostics_dialog.hpp"

#include "log_model.hpp"

#include <loglib/log_configuration.hpp>
#include <loglib/log_table.hpp>

#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QStringList>
#include <QStyle>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace
{
constexpr int DIALOG_INITIAL_WIDTH = 720;
constexpr int DIALOG_INITIAL_HEIGHT = 480;
constexpr int COL_HEADER = 0;
constexpr int COL_TYPE = 1;
constexpr int COL_AUTODETECT = 2;
constexpr int COL_TOTAL = 3;
constexpr int COL_PRESENT = 4;
constexpr int COL_MATCHING = 5;
constexpr int COL_MISMATCHED = 6;
constexpr int COL_PERCENT = 7;

QString FormatType(loglib::LogConfiguration::Type type)
{
    using Type = loglib::LogConfiguration::Type;
    switch (type)
    {
    case Type::Any:
        return QStringLiteral("Any");
    case Type::String:
        return QStringLiteral("String");
    case Type::Boolean:
        return QStringLiteral("Boolean");
    case Type::Integer:
        return QStringLiteral("Integer");
    case Type::Floating:
        return QStringLiteral("Floating-point");
    case Type::Number:
        return QStringLiteral("Number");
    case Type::Time:
        return QStringLiteral("Time");
    case Type::Enumeration:
        return QStringLiteral("Enumeration");
    case Type::Level:
        return QStringLiteral("Level");
    }
    return QStringLiteral("?");
}

QString FormatColumnHeader(const loglib::LogConfiguration::Column &column)
{
    QString text = QString::fromStdString(column.header);
    if (column.keys.size() > 1)
    {
        QStringList keys;
        keys.reserve(static_cast<int>(column.keys.size()));
        for (const auto &k : column.keys)
        {
            keys.append(QString::fromStdString(k));
        }
        text += QStringLiteral(" [%1]").arg(keys.join(QStringLiteral(",")));
    }
    else if (column.keys.size() == 1 && QString::fromStdString(column.keys.front()) != text)
    {
        text += QStringLiteral(" [%1]").arg(QString::fromStdString(column.keys.front()));
    }
    return text;
}

QTableWidgetItem *MakeReadOnlyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

QTableWidgetItem *MakeNumericItem(qulonglong value)
{
    auto *item = MakeReadOnlyItem(QString::number(value));
    item->setData(Qt::UserRole, value);
    item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return item;
}
} // namespace

ConfigurationDiagnosticsDialog::ConfigurationDiagnosticsDialog(LogModel *model, QWidget *parent)
    : QDialog(parent), mModel(model)
{
    setWindowTitle(tr("Configuration Diagnostics"));
    setObjectName(QStringLiteral("ConfigurationDiagnosticsDialog"));
    resize(DIALOG_INITIAL_WIDTH, DIALOG_INITIAL_HEIGHT);

    auto *layout = new QVBoxLayout(this);

    mSummaryLabel = new QLabel(this);
    mSummaryLabel->setWordWrap(true);
    layout->addWidget(mSummaryLabel);

    mTable = new QTableWidget(this);
    mTable->setObjectName(QStringLiteral("diagnosticsTable"));
    const QStringList headers{
        tr("Column"),
        tr("Configured type"),
        tr("Auto-detect"),
        tr("Total"),
        tr("Present"),
        tr("Matching"),
        tr("Mismatched"),
        tr("Mismatch %"),
    };
    mTable->setColumnCount(static_cast<int>(headers.size()));
    mTable->setHorizontalHeaderLabels(headers);
    mTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    mTable->setSelectionMode(QAbstractItemView::SingleSelection);
    mTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mTable->verticalHeader()->setVisible(false);
    mTable->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    mTable->horizontalHeader()->setStretchLastSection(false);
    mTable->setSortingEnabled(true);
    layout->addWidget(mTable, 1);

    auto *buttonRow = new QHBoxLayout();
    mRefreshButton = new QPushButton(tr("Refresh"), this);
    mRefreshButton->setObjectName(QStringLiteral("refreshButton"));
    buttonRow->addWidget(mRefreshButton);
    buttonRow->addStretch(1);
    mCloseButton = new QPushButton(tr("Close"), this);
    mCloseButton->setObjectName(QStringLiteral("closeButton"));
    mCloseButton->setDefault(true);
    buttonRow->addWidget(mCloseButton);
    layout->addLayout(buttonRow);

    connect(mRefreshButton, &QPushButton::clicked, this, [this]() {
        if (mModel)
        {
            mModel->RefreshColumnHealth();
        }
        Refresh();
    });
    connect(mCloseButton, &QPushButton::clicked, this, &QDialog::close);

    if (mModel)
    {
        connect(mModel, &LogModel::columnHealthChanged, this, &ConfigurationDiagnosticsDialog::Refresh);
    }

    Refresh();
}

void ConfigurationDiagnosticsDialog::Refresh()
{
    if (!mModel)
    {
        return;
    }

    const auto &columns = mModel->Configuration().columns;
    const auto wasSorting = mTable->isSortingEnabled();
    mTable->setSortingEnabled(false);
    mTable->clearContents();
    mTable->setRowCount(static_cast<int>(columns.size()));

    int mismatchedColumns = 0;
    for (int i = 0; i < static_cast<int>(columns.size()); ++i)
    {
        const auto &column = columns[static_cast<size_t>(i)];
        const auto healthOpt = mModel->ColumnHealth(i);
        const auto health = healthOpt.value_or(loglib::LogTable::ColumnTypeHealth{});

        const qulonglong total = health.totalSlots;
        const qulonglong present = health.presentSlots;
        const qulonglong matching = health.matchingSlots;
        const qulonglong mismatched = present > matching ? present - matching : 0;
        const double mismatchPct = present == 0 ? 0.0 : 100.0 * static_cast<double>(mismatched) / static_cast<double>(present);

        if (mismatched > 0)
        {
            ++mismatchedColumns;
        }

        auto *headerItem = MakeReadOnlyItem(FormatColumnHeader(column));
        if (mismatched > 0)
        {
            headerItem->setIcon(QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning));
        }
        mTable->setItem(i, COL_HEADER, headerItem);
        mTable->setItem(i, COL_TYPE, MakeReadOnlyItem(FormatType(column.type)));
        mTable->setItem(i, COL_AUTODETECT, MakeReadOnlyItem(column.autoDetect ? tr("Yes") : tr("No")));
        mTable->setItem(i, COL_TOTAL, MakeNumericItem(total));
        mTable->setItem(i, COL_PRESENT, MakeNumericItem(present));
        mTable->setItem(i, COL_MATCHING, MakeNumericItem(matching));
        mTable->setItem(i, COL_MISMATCHED, MakeNumericItem(mismatched));

        auto *pctItem = MakeReadOnlyItem(QStringLiteral("%1").arg(mismatchPct, 0, 'f', 1));
        pctItem->setData(Qt::UserRole, mismatchPct);
        pctItem->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
        mTable->setItem(i, COL_PERCENT, pctItem);

        if (mismatched > 0)
        {
            const QBrush highlight(QColor(255, 235, 235));
            for (int c = 0; c < mTable->columnCount(); ++c)
            {
                if (auto *item = mTable->item(i, c); item != nullptr)
                {
                    item->setBackground(highlight);
                }
            }
        }
    }

    mTable->setSortingEnabled(wasSorting);

    if (mismatchedColumns == 0)
    {
        mSummaryLabel->setText(tr("No configuration mismatches detected. Every column's values match its configured type."));
    }
    else
    {
        mSummaryLabel->setText(
            tr("%n column(s) have values that do not match the configured type. "
               "Either change the column's type in the Column Editor or leave auto-detect enabled.",
               nullptr,
               mismatchedColumns)
        );
    }
}

int ConfigurationDiagnosticsDialog::MismatchedColumnCount(const LogModel &model)
{
    const auto &columns = model.Configuration().columns;
    int mismatched = 0;
    for (int i = 0; i < static_cast<int>(columns.size()); ++i)
    {
        const auto health = model.ColumnHealth(i);
        if (!health.has_value())
        {
            continue;
        }
        if (health->presentSlots > health->matchingSlots)
        {
            ++mismatched;
        }
    }
    return mismatched;
}
