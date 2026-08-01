#include "export_dialog.hpp"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

#include <array>
#include <utility>

namespace
{

/// Soft warning threshold for Markdown exports. Above this, the
/// output becomes unwieldy in typical Markdown renderers.
constexpr std::size_t MARKDOWN_SOFT_WARNING_ROWS = 10000;

using ExportFormat = slv::exports::ExportFormat;

/// Format-catalogue view. `label` and `extension` are pulled from
/// `slv::exports` so the dialog, the completion toast, and the
/// documentation cannot drift out of sync. Only the file-dialog
/// filter string is dialog-local; it derives from the same
/// extension.
struct FormatEntry
{
    ExportFormat format;
    QString label;      // e.g. "JSON Lines"
    QString extension;  // e.g. "jsonl" (no leading dot)
    QString fileFilter; // e.g. "JSON Lines (*.jsonl);;All Files (*)"
};

std::array<FormatEntry, 4> BuildFormatEntries()
{
    constexpr std::array<ExportFormat, 4> ORDER = {
        ExportFormat::JsonLines,
        ExportFormat::Csv,
        ExportFormat::Snapshot,
        ExportFormat::Markdown,
    };
    std::array<FormatEntry, 4> entries;
    for (std::size_t i = 0; i < ORDER.size(); ++i)
    {
        const auto fmt = ORDER[i];
        const QString label = QString::fromLatin1(slv::exports::LabelFor(fmt));
        const QString ext = QString::fromLatin1(slv::exports::ExtensionFor(fmt));
        entries[i] = FormatEntry{
            .format = fmt,
            .label = label,
            .extension = ext,
            // "JSON Lines (*.jsonl);;All Files (*)"; single source of truth.
            .fileFilter = QStringLiteral("%1 (*.%2);;All Files (*)").arg(label, ext),
        };
    }
    return entries;
}

const std::array<FormatEntry, 4> &FormatEntries()
{
    static const std::array<FormatEntry, 4> ENTRIES = BuildFormatEntries();
    return ENTRIES;
}

const FormatEntry &EntryFor(ExportFormat format)
{
    for (const auto &entry : FormatEntries())
    {
        if (entry.format == format)
        {
            return entry;
        }
    }
    return FormatEntries().front();
}

bool StripKnownExtension(QString &path)
{
    for (const auto &entry : FormatEntries())
    {
        const QString suffix = QStringLiteral(".") + entry.extension;
        if (path.endsWith(suffix, Qt::CaseInsensitive))
        {
            path.chop(suffix.size());
            return true;
        }
    }
    return false;
}

} // namespace

ExportDialog::ExportDialog(
    std::size_t rowCountFiltered,
    std::size_t rowCountSelected,
    QString defaultStem,
    QString defaultDir,
    bool isLiveTail,
    QWidget *parent
)
    : QDialog(parent), mRowCountFiltered(rowCountFiltered), mRowCountSelected(rowCountSelected),
      mDefaultDir(std::move(defaultDir))
{
    setWindowTitle(tr("Export Filtered Rows"));
    setWindowModality(Qt::WindowModal);

    auto *layout = new QVBoxLayout(this);

    // Form: destination + format
    auto *form = new QFormLayout;
    layout->addLayout(form);

    mFormatCombo = new QComboBox(this);
    QSettings settings;
    const int savedFormat = settings.value(QStringLiteral("ui/lastExportFormat"), static_cast<int>(ExportFormat::JsonLines)).toInt();
    int selectedIndex = 0;
    const auto &entries = FormatEntries();
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const auto &entry = entries[i];
        // Combo entries read "JSON Lines (*.jsonl)" — the glob is a
        // hint at the suggested extension, useful even before the
        // user opens the Browse dialog.
        const QString displayLabel = QStringLiteral("%1 (*.%2)").arg(entry.label, entry.extension);
        mFormatCombo->addItem(displayLabel, static_cast<int>(entry.format));
        if (static_cast<int>(entry.format) == savedFormat)
        {
            selectedIndex = static_cast<int>(i);
        }
    }
    mFormatCombo->setCurrentIndex(selectedIndex);
    form->addRow(tr("&Format:"), mFormatCombo);

    // Destination path row: line edit + browse button
    auto *destRow = new QHBoxLayout;
    mDestinationEdit = new QLineEdit(this);
    if (defaultStem.isEmpty())
    {
        defaultStem = QStringLiteral("export");
    }
    const auto &initialEntry = EntryFor(static_cast<ExportFormat>(mFormatCombo->currentData().toInt()));
    QString initialPath = mDefaultDir;
    if (!initialPath.isEmpty() && !initialPath.endsWith(QDir::separator()) && !initialPath.endsWith(QLatin1Char('/')))
    {
        initialPath += QDir::separator();
    }
    initialPath += defaultStem + QStringLiteral(".") + initialEntry.extension;
    mDestinationEdit->setText(QDir::toNativeSeparators(initialPath));
    destRow->addWidget(mDestinationEdit);
    mBrowseButton = new QPushButton(tr("&Browse..."), this);
    destRow->addWidget(mBrowseButton);
    form->addRow(tr("&Destination:"), destRow);

    // Options
    auto *optionsLayout = new QVBoxLayout;
    layout->addLayout(optionsLayout);

    mSelectionOnly = new QCheckBox(tr("Export selection only"), this);
    mSelectionOnly->setChecked(false);
    if (rowCountSelected == 0)
    {
        mSelectionOnly->setEnabled(false);
        mSelectionOnly->setToolTip(tr("No rows are currently selected."));
    }
    optionsLayout->addWidget(mSelectionOnly);

    mIncludeHeader = new QCheckBox(tr("Include header row"), this);
    mIncludeHeader->setChecked(true);
    mIncludeHeader->setToolTip(tr("Applies to CSV and Markdown output."));
    optionsLayout->addWidget(mIncludeHeader);

    mIncludeHidden = new QCheckBox(tr("Include hidden columns"), this);
    mIncludeHidden->setChecked(false);
    mIncludeHidden->setToolTip(
        tr("By default, CSV and Markdown emit only visible columns. JSON Lines "
           "and Snapshot always include everything.")
    );
    optionsLayout->addWidget(mIncludeHidden);

    // Preview label
    mPreviewLabel = new QLabel(this);
    mPreviewLabel->setWordWrap(true);
    layout->addWidget(mPreviewLabel);

    mRowSizeWarning = new QLabel(this);
    mRowSizeWarning->setWordWrap(true);
    mRowSizeWarning->setStyleSheet(QStringLiteral("color: palette(shadow); font-style: italic;"));
    mRowSizeWarning->setVisible(false);
    layout->addWidget(mRowSizeWarning);

    mLiveTailNote = new QLabel(
        tr("Note: live-tail exports are a snapshot at start time. Rows arriving during the export are not included."),
        this
    );
    mLiveTailNote->setWordWrap(true);
    mLiveTailNote->setStyleSheet(QStringLiteral("color: palette(shadow); font-style: italic;"));
    mLiveTailNote->setVisible(isLiveTail);
    layout->addWidget(mLiveTailNote);

    // Buttons
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->button(QDialogButtonBox::Ok)->setText(tr("&Export"));
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, this, &ExportDialog::OnAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    connect(mFormatCombo, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &ExportDialog::OnFormatChanged);
    connect(mBrowseButton, &QPushButton::clicked, this, &ExportDialog::OnBrowseClicked);
    connect(mSelectionOnly, &QCheckBox::toggled, this, &ExportDialog::OnSelectionToggled);

    UpdateOptionVisibility();
    RefreshPreview();
}

ExportDialog::Config ExportDialog::Configuration() const
{
    Config cfg;
    cfg.format = static_cast<ExportFormat>(mFormatCombo->currentData().toInt());
    cfg.destination = mDestinationEdit->text();
    cfg.selectionOnly = mSelectionOnly->isEnabled() && mSelectionOnly->isChecked();
    cfg.includeHeaderRow = mIncludeHeader->isChecked();
    cfg.includeHiddenColumns = mIncludeHidden->isChecked();
    return cfg;
}

void ExportDialog::OnFormatChanged()
{
    UpdateExtensionSuggestion();
    UpdateOptionVisibility();
    RefreshPreview();
}

void ExportDialog::OnSelectionToggled()
{
    RefreshPreview();
}

void ExportDialog::OnBrowseClicked()
{
    const auto &entry = EntryFor(static_cast<ExportFormat>(mFormatCombo->currentData().toInt()));
    QString start = mDestinationEdit->text();
    if (start.isEmpty())
    {
        start = mDefaultDir;
    }
    const QString chosen = QFileDialog::getSaveFileName(
        this, tr("Choose Export Destination"), start, entry.fileFilter
    );
    if (chosen.isEmpty())
    {
        return;
    }
    mDestinationEdit->setText(QDir::toNativeSeparators(chosen));
}

void ExportDialog::OnAccept()
{
    const QString path = mDestinationEdit->text().trimmed();
    if (path.isEmpty())
    {
        QMessageBox::warning(this, tr("Export"), tr("Choose a destination file."));
        return;
    }
    QFileInfo info(path);
    if (info.isDir())
    {
        QMessageBox::warning(this, tr("Export"), tr("The destination points to a directory."));
        return;
    }
    if (!info.absoluteDir().exists())
    {
        QMessageBox::warning(
            this,
            tr("Export"),
            tr("Destination directory does not exist:\n%1").arg(info.absoluteDir().absolutePath())
        );
        return;
    }
    if (info.exists())
    {
        const auto choice = QMessageBox::question(
            this,
            tr("Overwrite File?"),
            tr("The file '%1' already exists. Overwrite it?").arg(info.fileName()),
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        if (choice != QMessageBox::Yes)
        {
            return;
        }
    }

    QSettings settings;
    settings.setValue(QStringLiteral("ui/lastExportFormat"), mFormatCombo->currentData().toInt());
    accept();
}

void ExportDialog::RefreshPreview()
{
    const std::size_t effective =
        (mSelectionOnly->isEnabled() && mSelectionOnly->isChecked()) ? mRowCountSelected : mRowCountFiltered;
    if (mSelectionOnly->isEnabled() && mSelectionOnly->isChecked())
    {
        mPreviewLabel->setText(
            tr("Will export %L1 row(s) (%L2 selected of %L3 in the current filter).")
                .arg(effective)
                .arg(mRowCountSelected)
                .arg(mRowCountFiltered)
        );
    }
    else
    {
        mPreviewLabel->setText(tr("Will export %L1 row(s) matching the current filter.").arg(effective));
    }

    const auto format = static_cast<ExportFormat>(mFormatCombo->currentData().toInt());
    if (format == ExportFormat::Markdown && effective > MARKDOWN_SOFT_WARNING_ROWS)
    {
        mRowSizeWarning->setText(
            tr("Markdown tables larger than %L1 rows can be slow or unusable in typical Markdown renderers.")
                .arg(MARKDOWN_SOFT_WARNING_ROWS)
        );
        mRowSizeWarning->setVisible(true);
    }
    else
    {
        mRowSizeWarning->setVisible(false);
    }
}

void ExportDialog::UpdateExtensionSuggestion()
{
    QString path = mDestinationEdit->text();
    if (path.isEmpty())
    {
        return;
    }
    if (!StripKnownExtension(path))
    {
        // Path has some unknown or missing extension; leave it alone.
        return;
    }
    const auto &entry = EntryFor(static_cast<ExportFormat>(mFormatCombo->currentData().toInt()));
    path += QStringLiteral(".") + entry.extension;
    mDestinationEdit->setText(path);
}

void ExportDialog::UpdateOptionVisibility()
{
    const auto format = static_cast<ExportFormat>(mFormatCombo->currentData().toInt());
    const bool isColumnFormat = (format == ExportFormat::Csv || format == ExportFormat::Markdown);
    mIncludeHeader->setEnabled(isColumnFormat);
    mIncludeHidden->setEnabled(isColumnFormat);
    if (!isColumnFormat)
    {
        mIncludeHeader->setToolTip(
            tr("Applies to CSV and Markdown output. JSON Lines emits all fields; Snapshot has no header.")
        );
        mIncludeHidden->setToolTip(
            tr("Applies to CSV and Markdown output. JSON Lines always includes every field; Snapshot is row-level.")
        );
    }
    else
    {
        mIncludeHeader->setToolTip(tr("Applies to CSV and Markdown output."));
        mIncludeHidden->setToolTip(tr("By default, CSV and Markdown emit only visible columns."));
    }
}
