#include "record_detail_widget.hpp"

#include "log_model.hpp"

#include <loglib/line_source.hpp>
#include <loglib/log_configuration.hpp>
#include <loglib/log_data.hpp>
#include <loglib/log_line.hpp>
#include <loglib/log_table.hpp>

#include <QApplication>
#include <QClipboard>
#include <QDebug>
#include <QFont>
#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QKeySequence>
#include <QLabel>
#include <QList>
#include <QMap>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QShortcut>
#include <QStringList>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTableWidgetSelectionRange>
#include <QTimer>
#include <QVBoxLayout>

#include <cstddef>
#include <exception>
#include <string>
#include <utility>

namespace
{
constexpr int OUTER_MARGIN = 12;
constexpr int SECTION_SPACING = 8;
constexpr int RAW_MIN_HEIGHT = 120;
constexpr int RAW_MAX_HEIGHT = 600;
constexpr int FIELDS_KEY_COLUMN = 0;
constexpr int FIELDS_VALUE_COLUMN = 1;
/// Render-side cap for the Raw JSON edit. Multi-megabyte log lines
/// would otherwise lock the UI for hundreds of ms in `fromJson` +
/// `setPlainText`. The clipboard payload (`rawJson`) is uncapped, so
/// "Copy raw JSON" still pushes every byte.
constexpr qsizetype RAW_FORMAT_INPUT_CAP_BYTES = 256 * 1024;
/// Debounce window (~one 60 Hz frame) for the post-resize row-height
/// reflow. Long enough to coalesce a fast drag's resize burst, short
/// enough that the wrap appears live.
constexpr int RESIZE_REFLOW_DEBOUNCE_MS = 16;

/// Pretty-print @p raw as indented JSON, or return the original bytes
/// (UTF-8) if it doesn't parse. Empty input returns an empty string.
/// Uses `fromRawData` for a zero-copy view; safe because neither
/// `QJsonDocument::fromJson` nor `QString::fromUtf8` retain the
/// pointer.
QString FormatRawLineForDisplay(const std::string &raw)
{
    if (raw.empty())
    {
        return {};
    }
    const auto rawSize = static_cast<qsizetype>(raw.size());
    if (rawSize > RAW_FORMAT_INPUT_CAP_BYTES)
    {
        // Skip the parse+indent for over-cap inputs and render a UTF-8
        // preview with a footer pointing at "Copy raw JSON". The
        // truncation may split a UTF-8 sequence; `fromUtf8` substitutes
        // U+FFFD, which is fine for a human-readable preview.
        QString truncated = QString::fromUtf8(raw.data(), RAW_FORMAT_INPUT_CAP_BYTES);
        truncated += QStringLiteral("\n\n");
        truncated += RecordDetailWidget::tr("... (%1 bytes truncated; use Copy raw JSON to retrieve the full content)")
                         .arg(static_cast<qulonglong>(rawSize - RAW_FORMAT_INPUT_CAP_BYTES));
        return truncated;
    }
    const QByteArray bytes = QByteArray::fromRawData(raw.data(), rawSize);
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error == QJsonParseError::NoError && (doc.isObject() || doc.isArray()))
    {
        return QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
    }
    return QString::fromUtf8(bytes);
}

/// C-style escape (`\n`, `\r`, `\t`, `\\`) for the `header: value`
/// clipboard output so each pair stays on one line. Other characters
/// pass through unchanged.
QString EscapeForKeyValueCopy(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar ch : text)
    {
        if (ch == QLatin1Char('\\'))
        {
            out += QStringLiteral("\\\\");
        }
        else if (ch == QLatin1Char('\n'))
        {
            out += QStringLiteral("\\n");
        }
        else if (ch == QLatin1Char('\r'))
        {
            out += QStringLiteral("\\r");
        }
        else if (ch == QLatin1Char('\t'))
        {
            out += QStringLiteral("\\t");
        }
        else
        {
            out += ch;
        }
    }
    return out;
}

/// Configure @p item as a read-only, bold key cell. Resets every
/// mutable property the placeholder path could have touched, so this
/// is safe to call on a recycled item too.
void ConfigureKeyItem(QTableWidgetItem *item, const QString &text)
{
    item->setText(text);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    item->setData(RECORD_DETAIL_EMPTY_PLACEHOLDER_ROLE, QVariant());
    item->setToolTip(QString());
    item->setForeground(QBrush());
    QFont font = item->font();
    font.setBold(true);
    font.setItalic(false);
    item->setFont(font);
}

/// Configure @p item as a read-only value cell. Empty input yields
/// the muted em-dash placeholder; non-empty resets every placeholder
/// property so a recycled cell doesn't keep the italic/muted look.
void ConfigureValueItem(QTableWidgetItem *item, const QString &text, const QPalette &palette)
{
    // Selectable for Ctrl+C copy via the widget's `QShortcut`.
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    QFont font = item->font();
    font.setBold(false);
    if (text.isEmpty())
    {
        // Muted em-dash with a role-flagged marker so consumers can
        // tell this placeholder from a real em-dash value. Palette
        // comes from the host so stylesheet / dark-mode overrides
        // drive the muted colour.
        item->setText(QStringLiteral("\u2014"));
        item->setToolTip(RecordDetailWidget::tr("This field is present but has an empty value."));
        item->setData(RECORD_DETAIL_EMPTY_PLACEHOLDER_ROLE, true);
        font.setItalic(true);
        item->setForeground(palette.color(QPalette::PlaceholderText));
    }
    else
    {
        item->setText(text);
        item->setToolTip(QString());
        item->setData(RECORD_DETAIL_EMPTY_PLACEHOLDER_ROLE, QVariant());
        font.setItalic(false);
        // Default foreground so a recycled placeholder cell loses
        // its muted colour.
        item->setForeground(QBrush());
    }
    item->setFont(font);
}
} // namespace

QString DefaultRecordDetailPlaceholder()
{
    // `RecordDetailWidget::tr` (not `QObject::tr`) so translators see
    // this screen's strings grouped together.
    return RecordDetailWidget::tr(
        "Select a row in the table to inspect it here, or double-click any row to open this pane."
    );
}

QString EvictedRecordPlaceholder()
{
    // Used by the FIFO-eviction path; `modelReset` deliberately
    // routes through `Clear()` and shows the default placeholder
    // instead.
    return RecordDetailWidget::tr("The pinned record is no longer available (evicted from a streaming buffer).");
}

RecordDetailContent BuildRecordDetailContent(const LogModel &model, int sourceRow)
{
    RecordDetailContent content;

    if (sourceRow < 0 || sourceRow >= model.rowCount())
    {
        content.placeholderText = DefaultRecordDetailPlaceholder();
        return content;
    }

    const auto row = static_cast<size_t>(sourceRow);
    const auto &table = model.Table();
    const auto &configuration = model.Configuration();
    const auto &lines = table.Data().Lines();
    // Belt-and-braces against future divergence between `rowCount()`
    // and `Lines().size()`.
    if (row >= lines.size())
    {
        content.placeholderText = EvictedRecordPlaceholder();
        return content;
    }

    const auto &line = lines[row];
    content.fields.reserve(static_cast<int>(configuration.columns.size()));
    for (size_t col = 0; col < configuration.columns.size(); ++col)
    {
        const QString header = QString::fromStdString(configuration.columns[col].header);
        const QString value = QString::fromStdString(table.GetFormattedValue(row, col));
        content.fields.emplace_back(header, value);
    }

    std::string rawLineBytes;
    if (const loglib::LineSource *source = line.Source(); source != nullptr)
    {
        try
        {
            rawLineBytes = source->RawLine(line.LineId());
        }
        catch (const std::exception &e)
        {
            // Line source threw (streaming eviction or other failure).
            // Keep the parsed fields visible; log so unexpected
            // failure modes are observable.
            qWarning() << "BuildRecordDetailContent: line source threw for row" << sourceRow << "line id"
                       << static_cast<qulonglong>(line.LineId()) << ":" << e.what();
            rawLineBytes.clear();
        }
    }
    content.rawJson = QString::fromUtf8(rawLineBytes.data(), static_cast<qsizetype>(rawLineBytes.size()));
    content.formattedJson = FormatRawLineForDisplay(rawLineBytes);

    QString summary = RecordDetailWidget::tr("Row %1").arg(sourceRow + 1);
    if (const int timeCol = loglib::FirstTimeColumnIndex(configuration); timeCol >= 0)
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
        // Muted tone so placeholder text doesn't compete with real content.
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
    // Spreadsheet-style: Ctrl-click toggles cells, Shift-click extends.
    mFieldsTable->setSelectionMode(QAbstractItemView::ExtendedSelection);
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
    // Wrap at widget width; the `RAW_MAX_HEIGHT` cap keeps long
    // payloads from pushing the buttons row off-screen.
    mRawEdit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    mRawEdit->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    mRawEdit->setMinimumHeight(RAW_MIN_HEIGHT);
    mRawEdit->setMaximumHeight(RAW_MAX_HEIGHT);
    rawLayout->addWidget(mRawEdit);
    // Show/hide the edit with the group box's checked state.
    connect(mRawGroup, &QGroupBox::toggled, mRawEdit, &QWidget::setVisible);
    // Record the user's expand intent so a follow-up no-raw record
    // (force-collapsed below) doesn't overwrite it. Programmatic
    // toggles set `mSuppressRawToggleHandler` to skip this.
    connect(mRawGroup, &QGroupBox::toggled, this, [this](bool on) {
        if (!mSuppressRawToggleHandler)
        {
            mUserPrefersRawExpanded = on;
        }
    });
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

    // Ctrl+C inside the fields table copies as TSV. Scope to the
    // table so it doesn't shadow the host window's Ctrl+C.
    auto *copyShortcut = new QShortcut(QKeySequence::Copy, mFieldsTable);
    copyShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(copyShortcut, &QShortcut::activated, this, &RecordDetailWidget::CopyFieldsSelectionToClipboard);

    // Debounced row-height reflow on width changes (see member docstring).
    mResizeReflowTimer = new QTimer(this);
    mResizeReflowTimer->setSingleShot(true);
    mResizeReflowTimer->setInterval(RESIZE_REFLOW_DEBOUNCE_MS);
    connect(mResizeReflowTimer, &QTimer::timeout, this, [this]() {
        if (mFieldsTable->rowCount() > 0)
        {
            mFieldsTable->resizeRowsToContents();
        }
    });

    PopulateUi();
}

void RecordDetailWidget::SetContent(const RecordDetailContent &content)
{
    mContent = content;
    PopulateUi();
}

void RecordDetailWidget::SetOpenInNewWindowVisible(bool visible)
{
    mOpenInNewWindowButton->setVisible(visible);
}

void RecordDetailWidget::PopulateUi()
{
    const bool hasContent = mContent.valid;
    mSummaryLabel->setVisible(hasContent);
    mFieldsTable->setVisible(hasContent);
    mRawGroup->setVisible(hasContent);
    // Gate on having raw bytes too: an evicted line can produce a
    // valid content with empty `rawJson`, where a click would no-op.
    mCopyJsonButton->setEnabled(hasContent && !mContent.rawJson.isEmpty());
    mCopyKvButton->setEnabled(hasContent);
    mOpenInNewWindowButton->setEnabled(hasContent);
    mPlaceholderLabel->setVisible(!hasContent);

    if (!hasContent)
    {
        mPlaceholderLabel->setText(
            mContent.placeholderText.isEmpty() ? DefaultRecordDetailPlaceholder() : mContent.placeholderText
        );
        mFieldsTable->setRowCount(0);
        mRawEdit->clear();
        mSummaryLabel->clear();
        return;
    }

    mSummaryLabel->setText(mContent.summary);

    // Recycle items in place when the row count is unchanged.
    // Arrow-key navigation across rows of the same shape would
    // otherwise allocate 2N items per step, visibly stuttering on
    // wide schemas. The configure helpers reset every placeholder
    // property, so recycled items don't carry stale state.
    const int newRowCount = static_cast<int>(mContent.fields.size());
    const int oldRowCount = mFieldsTable->rowCount();
    if (newRowCount != oldRowCount)
    {
        mFieldsTable->setRowCount(newRowCount);
    }
    // Snapshot the host palette so cell builders see parent overrides
    // and we don't re-fetch it per cell.
    const QPalette tablePalette = mFieldsTable->palette();
    for (int i = 0; i < newRowCount; ++i)
    {
        const auto &pair = mContent.fields[i];
        QTableWidgetItem *keyItem = mFieldsTable->item(i, FIELDS_KEY_COLUMN);
        if (keyItem == nullptr)
        {
            keyItem = new QTableWidgetItem();
            mFieldsTable->setItem(i, FIELDS_KEY_COLUMN, keyItem);
        }
        ConfigureKeyItem(keyItem, pair.first);
        QTableWidgetItem *valueItem = mFieldsTable->item(i, FIELDS_VALUE_COLUMN);
        if (valueItem == nullptr)
        {
            valueItem = new QTableWidgetItem();
            mFieldsTable->setItem(i, FIELDS_VALUE_COLUMN, valueItem);
        }
        ConfigureValueItem(valueItem, pair.second, tablePalette);
    }
    // Wraps + explicit newlines drive row height; rerun on resize so
    // a column-width change reflows.
    mFieldsTable->resizeRowsToContents();

    mRawEdit->setPlainText(mContent.formattedJson);
    // No raw bytes (eviction, line source threw, truncated file, ...)
    // -> disable + retitle the group so the empty state is visible,
    // not silently hidden behind a collapsed toggle. The user's
    // expansion preference is held in `mUserPrefersRawExpanded` and
    // restored on the next record with raw bytes.
    const bool hasRaw = !mContent.rawJson.isEmpty();
    const bool desiredChecked = hasRaw && mUserPrefersRawExpanded;
    if (mRawGroup->isChecked() != desiredChecked)
    {
        // Suppress the user-preference recorder during this
        // programmatic flip; the visibility hook still runs.
        mSuppressRawToggleHandler = true;
        mRawGroup->setChecked(desiredChecked);
        mSuppressRawToggleHandler = false;
    }
    else
    {
        // State unchanged, so `toggled` didn't fire; sync explicitly
        // (covers the initial-render path).
        mRawEdit->setVisible(desiredChecked);
    }
    mRawGroup->setEnabled(hasRaw);
    mRawGroup->setTitle(hasRaw ? tr("Raw JSON") : tr("Raw JSON (unavailable)"));
    mRawGroup->setToolTip(hasRaw ? QString() : tr("The original line bytes are no longer available for this record."));
}

void RecordDetailWidget::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    // Only width changes affect the wrap point. Debounced via
    // `mResizeReflowTimer` so a fast drag's resize burst collapses
    // into one `resizeRowsToContents` call.
    if (mFieldsTable->rowCount() > 0 && event->oldSize().width() != event->size().width())
    {
        mResizeReflowTimer->start();
    }
}

void RecordDetailWidget::RefreshPalette()
{
    // The placeholder label has an explicit foreground override
    // so it doesn't track palette changes on its own. Read the
    // theme colour from the app palette (not this label's own,
    // which carries the override).
    QPalette placeholderPalette = mPlaceholderLabel->palette();
    placeholderPalette.setColor(mPlaceholderLabel->foregroundRole(), qApp->palette().color(QPalette::PlaceholderText));
    mPlaceholderLabel->setPalette(placeholderPalette);

    // Rebuild cells so placeholder-row foregrounds re-pick the
    // theme colour. No-op when there's no displayed content.
    PopulateUi();
}

void RecordDetailWidget::CopyAsJsonClicked() const
{
    if (!mContent.valid || mContent.rawJson.isEmpty())
    {
        return;
    }
    // Copy the on-disk bytes, not the pretty-printed display form,
    // so pasting round-trips the JSON the parser saw.
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
        // Escape both sides so embedded newlines / tabs don't break
        // the one-line-per-field structure on the receiver.
        lines.append(
            QStringLiteral("%1: %2").arg(EscapeForKeyValueCopy(pair.first), EscapeForKeyValueCopy(pair.second))
        );
    }
    QApplication::clipboard()->setText(lines.join(QLatin1Char('\n')));
}

void RecordDetailWidget::CopyFieldsSelectionToClipboard() const
{
    const QList<QTableWidgetSelectionRange> ranges = mFieldsTable->selectedRanges();
    if (ranges.isEmpty())
    {
        return;
    }
    // Coalesce selection rectangles into per-row column sets. Under
    // `ExtendedSelection`, Qt returns one range per Ctrl-click and
    // doesn't merge them, so a naive emit duplicates rows and can
    // emit cells in click order. `QMap<int, QSet<int>>` sorts rows
    // and forces (key, value) column order.
    //
    // Reading from `mContent.fields` keeps the em-dash placeholder
    // out of the clipboard -- we copy the underlying text.
    QMap<int, QSet<int>> selectedCells;
    for (const QTableWidgetSelectionRange &range : ranges)
    {
        for (int row = range.topRow(); row <= range.bottomRow(); ++row)
        {
            if (row < 0 || row >= static_cast<int>(mContent.fields.size()))
            {
                continue;
            }
            for (int col = range.leftColumn(); col <= range.rightColumn(); ++col)
            {
                if (col != FIELDS_KEY_COLUMN && col != FIELDS_VALUE_COLUMN)
                {
                    continue;
                }
                selectedCells[row].insert(col);
            }
        }
    }
    if (selectedCells.isEmpty())
    {
        return;
    }
    QStringList rowLines;
    rowLines.reserve(static_cast<int>(selectedCells.size()));
    for (auto it = selectedCells.cbegin(); it != selectedCells.cend(); ++it)
    {
        const int row = it.key();
        QStringList cells;
        if (it.value().contains(FIELDS_KEY_COLUMN))
        {
            cells.append(mContent.fields[row].first);
        }
        if (it.value().contains(FIELDS_VALUE_COLUMN))
        {
            cells.append(mContent.fields[row].second);
        }
        rowLines.append(cells.join(QLatin1Char('\t')));
    }
    QApplication::clipboard()->setText(rowLines.join(QLatin1Char('\n')));
}
