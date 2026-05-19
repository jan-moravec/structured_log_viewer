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

#include <utility>

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

/// Item that sorts numerically (using `Qt::UserRole`) so "10" beats
/// "2" instead of losing on string comparison.
class NumericTableWidgetItem : public QTableWidgetItem
{
public:
    NumericTableWidgetItem(const QString &display, double sortValue)
        : QTableWidgetItem(display)
    {
        setFlags(flags() & ~Qt::ItemIsEditable);
        setData(Qt::UserRole, sortValue);
        setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    }

    [[nodiscard]] bool operator<(const QTableWidgetItem &other) const override
    {
        const QVariant lhs = data(Qt::UserRole);
        const QVariant rhs = other.data(Qt::UserRole);
        if (lhs.isValid() && rhs.isValid())
        {
            return lhs.toDouble() < rhs.toDouble();
        }
        return QTableWidgetItem::operator<(other);
    }
};

QTableWidgetItem *MakeNumericItem(qulonglong value)
{
    return new NumericTableWidgetItem(QString::number(value), static_cast<double>(value));
}

QTableWidgetItem *MakePercentItem(double value)
{
    return new NumericTableWidgetItem(QStringLiteral("%1").arg(value, 0, 'f', 1), value);
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
        // The cache recompute below emits `columnHealthChanged`,
        // which is already wired to `Refresh()` -- avoids a double
        // rebuild on the common "no change" path.
        if (mModel)
        {
            mModel->RefreshColumnHealth();
        }
        else
        {
            Refresh();
        }
    });
    connect(mCloseButton, &QPushButton::clicked, this, &QDialog::close);

    // Double-click drills into the column editor. The source column
    // index lives in the row's first item's `Qt::UserRole` so it
    // survives user-driven sorting.
    connect(mTable, &QTableWidget::cellDoubleClicked, this, [this](int row, int /*col*/) {
        if (auto *item = mTable->item(row, COL_HEADER); item != nullptr)
        {
            const QVariant payload = item->data(Qt::UserRole);
            bool ok = false;
            const int columnIndex = payload.toInt(&ok);
            if (ok && columnIndex >= 0)
            {
                emit editColumnRequested(columnIndex);
            }
        }
    });

    if (mModel)
    {
        // Skip rebuilds while hidden -- frequent streaming
        // promote/demote events would otherwise churn O(columns)
        // per batch on a widget the user can't see.
        connect(mModel, &LogModel::columnHealthChanged, this, [this]() {
            if (isVisible())
            {
                Refresh();
            }
        });
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
    for (int i = 0; std::cmp_less(i, columns.size()); ++i)
    {
        const auto &column = columns[static_cast<size_t>(i)];
        const auto healthOpt = mModel->ColumnHealth(i);
        const auto health = healthOpt.value_or(loglib::LogTable::ColumnTypeHealth{});

        const qulonglong total = health.totalSlots;
        const qulonglong present = health.presentSlots;
        const qulonglong matching = health.matchingSlots;
        const qulonglong mismatched = present > matching ? present - matching : 0;
        const double mismatchPct =
            present == 0 ? 0.0 : 100.0 * static_cast<double>(mismatched) / static_cast<double>(present);

        if (mismatched > 0)
        {
            ++mismatchedColumns;
        }

        auto *headerItem = MakeReadOnlyItem(FormatColumnHeader(column));
        if (mismatched > 0)
        {
            headerItem->setIcon(QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning));
        }
        // Stash the source-table index so the double-click drill-down
        // works after user sorting.
        headerItem->setData(Qt::UserRole, i);
        mTable->setItem(i, COL_HEADER, headerItem);
        mTable->setItem(i, COL_TYPE, MakeReadOnlyItem(FormatType(column.type)));
        mTable->setItem(i, COL_AUTODETECT, MakeReadOnlyItem(column.autoDetect ? tr("Yes") : tr("No")));
        mTable->setItem(i, COL_TOTAL, MakeNumericItem(total));
        mTable->setItem(i, COL_PRESENT, MakeNumericItem(present));
        mTable->setItem(i, COL_MATCHING, MakeNumericItem(matching));
        mTable->setItem(i, COL_MISMATCHED, MakeNumericItem(mismatched));

        mTable->setItem(i, COL_PERCENT, MakePercentItem(mismatchPct));

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
        mSummaryLabel->setText(
            tr("No configuration mismatches detected. Every column's values match its configured type.")
        );
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
    for (int i = 0; std::cmp_less(i, columns.size()); ++i)
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
