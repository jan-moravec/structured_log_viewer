#include "record_detail_widget.hpp"

#include "log_model.hpp"

#include <loglib/line_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_table.hpp>

#include <QApplication>
#include <QClipboard>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLabel>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>

namespace
{
constexpr int OUTER_MARGIN = 12;
constexpr int SECTION_SPACING = 8;
constexpr int ROW_VERTICAL_PADDING = 4;
constexpr int RAW_MIN_HEIGHT = 120;
constexpr int RAW_MAX_HEIGHT = 320;
constexpr int FIELDS_KEY_COLUMN = 0;
constexpr int FIELDS_VALUE_COLUMN = 1;

QString PrettyPrintJson(const std::string &raw)
{
    if (raw.empty())
    {
        return QString();
    }
    const QByteArray bytes = QByteArray::fromStdString(raw);
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error == QJsonParseError::NoError && (doc.isObject() || doc.isArray()))
    {
        return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
    }
    // Non-JSON / malformed: fall back to the original bytes so the
    // user still sees something usable.
    return QString::fromUtf8(bytes);
}

/// Pick the timestamp column (first `Type::Time` column) so the
/// summary label can show `Row N · <timestamp>`. Returns -1 when no
/// time column exists.
int FindTimeColumn(const loglib::LogConfiguration &configuration)
{
    for (size_t i = 0; i < configuration.columns.size(); ++i)
    {
        if (configuration.columns[i].type == loglib::LogConfiguration::Type::Time)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

QTableWidgetItem *MakeReadOnlyKeyItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    QFont font = item->font();
    font.setBold(true);
    item->setFont(font);
    return item;
}

QTableWidgetItem *MakeReadOnlyValueItem(const QString &text)
{
    auto *item = new QTableWidgetItem(text);
    // Selectable so the user can copy a single value with Ctrl+C
    // inside the table, but not editable.
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    if (text.isEmpty())
    {
        // Muted placeholder for missing values; reads "<empty>" so
        // an empty cell isn't visually identical to a present-but-blank
        // value the parser produced.
        item->setText(QStringLiteral("<empty>"));
        QFont font = item->font();
        font.setItalic(true);
        item->setFont(font);
        QPalette palette;
        item->setForeground(palette.color(QPalette::PlaceholderText));
    }
    return item;
}
} // namespace

RecordDetailContent BuildRecordDetailContent(const LogModel &model, int sourceRow)
{
    RecordDetailContent content;

    if (sourceRow < 0 || sourceRow >= model.rowCount())
    {
        content.placeholderText =
            QObject::tr("Select a row in the table to inspect it here, or double-click any row to open this pane.");
        return content;
    }

    const auto row = static_cast<size_t>(sourceRow);
    const auto &table = model.Table();
    const auto &configuration = model.Configuration();
    const auto &lines = table.Data().Lines();
    if (row >= lines.size())
    {
        // Row index ahead of the line store -- happens transiently
        // during a streaming append between `beginInsertRows` and
        // the model emitting `dataChanged`. Treat as evicted /
        // unavailable so the placeholder is friendly.
        content.placeholderText = QObject::tr("This record is no longer available.");
        return content;
    }

    const auto &line = lines[row];
    content.fields.reserve(static_cast<int>(configuration.columns.size()));
    for (size_t col = 0; col < configuration.columns.size(); ++col)
    {
        const QString header = QString::fromStdString(configuration.columns[col].header);
        // Multi-line nested values are preserved as compact JSON by
        // `GetFormattedValue`; we keep them as-is so the value cell
        // can wrap them.
        const QString value = QString::fromStdString(table.GetFormattedValue(row, col));
        content.fields.emplace_back(header, value);
    }

    std::string rawLineBytes;
    if (loglib::LineSource *source = line.Source(); source != nullptr)
    {
        try
        {
            rawLineBytes = source->RawLine(line.LineId());
        }
        catch (const std::out_of_range &)
        {
            // Evicted from a streaming source: keep the parsed fields
            // visible but mark the raw section empty.
            rawLineBytes.clear();
        }
    }
    content.rawJson = PrettyPrintJson(rawLineBytes);

    QString summary = QObject::tr("Row %1").arg(sourceRow + 1);
    if (const int timeCol = FindTimeColumn(configuration); timeCol >= 0)
    {
        const QString timeText = QString::fromStdString(table.GetFormattedValue(row, static_cast<size_t>(timeCol)));
        if (!timeText.isEmpty())
        {
            summary = QStringLiteral("%1 \u00B7 %2").arg(summary, timeText);
        }
    }
    content.summary = summary;
    content.valid = true;
    return content;
}

RecordDetailWidget::RecordDetailWidget(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("RecordDetailWidget"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(OUTER_MARGIN, OUTER_MARGIN, OUTER_MARGIN, OUTER_MARGIN);
    layout->setSpacing(SECTION_SPACING);

    mSummaryLabel = new QLabel(this);
    mSummaryLabel->setObjectName(QStringLiteral("summaryLabel"));
    mSummaryLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    QFont summaryFont = mSummaryLabel->font();
    summaryFont.setBold(true);
    mSummaryLabel->setFont(summaryFont);
    layout->addWidget(mSummaryLabel);

    mPlaceholderLabel = new QLabel(this);
    mPlaceholderLabel->setObjectName(QStringLiteral("placeholderLabel"));
    mPlaceholderLabel->setWordWrap(true);
    {
        // Muted-foreground tone so the placeholder text doesn't
        // compete with real content when the pane has something to
        // show.
        QPalette palette = mPlaceholderLabel->palette();
        palette.setColor(mPlaceholderLabel->foregroundRole(), palette.color(QPalette::PlaceholderText));
        mPlaceholderLabel->setPalette(palette);
    }
    layout->addWidget(mPlaceholderLabel);

    mFieldsTable = new QTableWidget(this);
    mFieldsTable->setObjectName(QStringLiteral("fieldsTable"));
    mFieldsTable->setColumnCount(2);
    mFieldsTable->setHorizontalHeaderLabels({tr("Field"), tr("Value")});
    mFieldsTable->verticalHeader()->setVisible(false);
    mFieldsTable->setSelectionBehavior(QAbstractItemView::SelectItems);
    mFieldsTable->setSelectionMode(QAbstractItemView::ContiguousSelection);
    mFieldsTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    mFieldsTable->setAlternatingRowColors(true);
    mFieldsTable->setShowGrid(false);
    mFieldsTable->setWordWrap(true);
    mFieldsTable->setTextElideMode(Qt::ElideNone);
    mFieldsTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    mFieldsTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    {
        QHeaderView *hHeader = mFieldsTable->horizontalHeader();
        QFont headerFont = hHeader->font();
        headerFont.setBold(true);
        hHeader->setFont(headerFont);
        hHeader->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
        hHeader->setHighlightSections(false);
        hHeader->setSectionResizeMode(FIELDS_KEY_COLUMN, QHeaderView::ResizeToContents);
        hHeader->setSectionResizeMode(FIELDS_VALUE_COLUMN, QHeaderView::Stretch);
        hHeader->setStretchLastSection(true);
    }
    layout->addWidget(mFieldsTable, 1);

    mRawGroup = new QGroupBox(tr("Raw JSON"), this);
    mRawGroup->setObjectName(QStringLiteral("rawJsonGroup"));
    mRawGroup->setCheckable(true);
    mRawGroup->setChecked(false);
    auto *rawLayout = new QVBoxLayout(mRawGroup);
    rawLayout->setContentsMargins(SECTION_SPACING, SECTION_SPACING, SECTION_SPACING, SECTION_SPACING);
    mRawEdit = new QPlainTextEdit(mRawGroup);
    mRawEdit->setObjectName(QStringLiteral("rawJsonEdit"));
    mRawEdit->setReadOnly(true);
    mRawEdit->setLineWrapMode(QPlainTextEdit::NoWrap);
    mRawEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    mRawEdit->setMinimumHeight(RAW_MIN_HEIGHT);
    mRawEdit->setMaximumHeight(RAW_MAX_HEIGHT);
    rawLayout->addWidget(mRawEdit);
    // Toggling the group box collapses everything inside; we use that
    // to hide the QPlainTextEdit on demand without messing with the
    // outer layout.
    connect(mRawGroup, &QGroupBox::toggled, mRawEdit, &QWidget::setVisible);
    mRawEdit->setVisible(false);
    layout->addWidget(mRawGroup);

    auto *buttonRow = new QHBoxLayout();
    buttonRow->setSpacing(SECTION_SPACING);
    mCopyJsonButton = new QPushButton(tr("Copy raw JSON"), this);
    mCopyJsonButton->setObjectName(QStringLiteral("copyJsonButton"));
    mCopyKvButton = new QPushButton(tr("Copy as key/value"), this);
    mCopyKvButton->setObjectName(QStringLiteral("copyKeyValueButton"));
    mOpenInNewWindowButton = new QPushButton(tr("Open in new window"), this);
    mOpenInNewWindowButton->setObjectName(QStringLiteral("openInNewWindowButton"));
    buttonRow->addWidget(mCopyJsonButton);
    buttonRow->addWidget(mCopyKvButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(mOpenInNewWindowButton);
    layout->addLayout(buttonRow);

    connect(mCopyJsonButton, &QPushButton::clicked, this, &RecordDetailWidget::CopyAsJsonClicked);
    connect(mCopyKvButton, &QPushButton::clicked, this, &RecordDetailWidget::CopyAsKeyValueClicked);
    connect(mOpenInNewWindowButton, &QPushButton::clicked, this, &RecordDetailWidget::openInNewWindowRequested);

    PopulateUi();
}

void RecordDetailWidget::SetContent(const RecordDetailContent &content)
{
    mContent = content;
    PopulateUi();
}

void RecordDetailWidget::SetOpenInNewWindowVisible(bool visible)
{
    if (mOpenInNewWindowButton != nullptr)
    {
        mOpenInNewWindowButton->setVisible(visible);
    }
}

void RecordDetailWidget::PopulateUi()
{
    const bool hasContent = mContent.valid;
    mSummaryLabel->setVisible(hasContent);
    mFieldsTable->setVisible(hasContent);
    mRawGroup->setVisible(hasContent);
    mCopyJsonButton->setEnabled(hasContent);
    mCopyKvButton->setEnabled(hasContent);
    mOpenInNewWindowButton->setEnabled(hasContent);
    mPlaceholderLabel->setVisible(!hasContent);

    if (!hasContent)
    {
        mPlaceholderLabel->setText(
            mContent.placeholderText.isEmpty()
                ? tr("Select a row in the table to inspect it here, or double-click any row to open this pane.")
                : mContent.placeholderText
        );
        mFieldsTable->setRowCount(0);
        mRawEdit->clear();
        mSummaryLabel->clear();
        return;
    }

    mSummaryLabel->setText(mContent.summary);

    mFieldsTable->setRowCount(static_cast<int>(mContent.fields.size()));
    const int derivedRowHeight = mFieldsTable->fontMetrics().height() + (ROW_VERTICAL_PADDING * 2);
    for (int i = 0; i < static_cast<int>(mContent.fields.size()); ++i)
    {
        const auto &pair = mContent.fields[i];
        mFieldsTable->setItem(i, FIELDS_KEY_COLUMN, MakeReadOnlyKeyItem(pair.first));
        mFieldsTable->setItem(i, FIELDS_VALUE_COLUMN, MakeReadOnlyValueItem(pair.second));
        // Grow row height for multi-line values so the cell shows
        // everything without scrolling internally.
        const int newlines = static_cast<int>(pair.second.count(QLatin1Char('\n')));
        const int rowHeight = newlines > 0 ? derivedRowHeight * (newlines + 1) : derivedRowHeight;
        mFieldsTable->setRowHeight(i, rowHeight);
    }

    mRawEdit->setPlainText(mContent.rawJson);
    // Empty raw JSON (eviction / parse failure with empty bytes)
    // collapses the group implicitly because the toggle below
    // hides the edit; we leave the group visible so the user can
    // see that there is no raw text available.
}

void RecordDetailWidget::CopyAsJsonClicked() const
{
    if (!mContent.valid || mContent.rawJson.isEmpty())
    {
        return;
    }
    QApplication::clipboard()->setText(mContent.rawJson);
}

void RecordDetailWidget::CopyAsKeyValueClicked() const
{
    if (!mContent.valid)
    {
        return;
    }
    QStringList lines;
    lines.reserve(static_cast<int>(mContent.fields.size()));
    for (const auto &pair : mContent.fields)
    {
        lines.append(QStringLiteral("%1: %2").arg(pair.first, pair.second));
    }
    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
}
